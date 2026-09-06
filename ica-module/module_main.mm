// Plan 2 Task 2 — ICD_Scanner session / open callbacks.
//
// Plan 2 Task 1 proved the module LOADS: icdd matches the synthetic device,
// launches this ad-hoc-signed bundle, and runs main() (see
// docs/RUNBOOK-plan-2-loadspike.md). But selecting the device in Image Capture
// returned "Failed to open a connection to the device (-21345)" —
// ICReturnConnectionFailedToOpen (ImageCaptureCore/ImageCaptureConstants.h) —
// because the module's callbacks were inert stubs: ICD_ScannerOpenTCPIPDevice
// returned noErr but left the ScannerObjectInfo zeroed, so icdd could not build
// a device object to connect to.
//
// This task implements the session / description layer: open a connection to
// the (network) device, open/close a scanner session, report device status,
// and describe the scan parameters (resolutions, colour modes, sources, paper
// sizes). That is the bar for this task — the device opens and shows a scan
// panel. It does NOT run a real pixel scan: ICD_ScannerStart returns a clean
// "unsupported" status (no crash); RunScan wiring is a later task (PLAN-2
// task 7).
//
// Callback set + signatures come from the public SDK only:
//   ICADevices.framework/Headers/ICD_ScannerCalls.h  — the callback typedefs,
//     the ScannerObjectInfo struct, the ICD_Scanner*PB parameter blocks, the
//     gICDScannerCallbackFunctions table, and ICD_ScannerMain / the
//     ICDScannerGetStandardPropertyData helper.
//   ICADevices.framework/Headers/ICAApplication.h    — ICAObjectInfo, the
//     kICADevice / kICADeviceScanner object types, kICAIndexOutOfRangeErr /
//     kICADeviceUnsupportedErr, and the kICAIP*/kICABonjour* param-dict keys.
//   ImageCaptureCore.framework/Headers/ImageCaptureConstants.h — the -21345
//     ICReturnConnectionFailedToOpen constant this task clears (client side;
//     read only to identify the error, not linked).
//
// Clean-room: written only against those public SDK headers plus the device's
// own black-box behaviour; no Brother or Apple source. Device identity is the
// synthetic BRW00AABBCCDDEE.

#import <Foundation/Foundation.h>
#import <ICADevices/ICADevices.h>
#import <os/log.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "brscan/scanner.h"
#include "brscan/transport_tcp.h"
#include "brscan/types.h"
#include "buffer_descriptor.h"
#include "decode_jpeg.h"  // libbrscan private header (on the libbrscan inc dir).
#include "scan_parameters.h"
#include "scan_translate.h"

namespace {

// Rate-limited tracing for callbacks icdd may POLL. A polled callback that
// os_log()s on every invocation floods the unified log (Plan 2 Task 2 shipped a
// per-call os_log on ICD_ScannerGetObjectInfo, which spewed hundreds of lines a
// millisecond once icdd started walking the object tree). LOG_ONCE fires a given
// call site's message exactly once for the life of the process, so the
// open/session/parameter path stays traceable while the polled paths go quiet
// after their first line. Each expansion gets its own private static flag.
#define LOG_ONCE(fmt, ...)                                       \
  do {                                                           \
    static std::atomic<bool> _brscan_logged{false};             \
    if (!_brscan_logged.exchange(true)) {                        \
      os_log(Log(), fmt, ##__VA_ARGS__);                         \
    }                                                            \
  } while (0)

// os_log tracing. Keep the subsystem the runbook already streams
// (me.tthoma24.brscan.ica); the "session" category marks this task's callbacks
// so a live re-test can see, in order, which entry points icdd drives and that
// a connection + session opened without -21345. Stream with:
//
//   log stream --predicate 'subsystem == "me.tthoma24.brscan.ica"' --info --debug
os_log_t Log() {
  static os_log_t log = os_log_create("me.tthoma24.brscan.ica", "session");
  return log;
}

// Synthetic per-unit identity used when the host does not hand us a name.
constexpr char kSyntheticDeviceName[] = "BRW00AABBCCDDEE";

// Raw-scan TCP port the Brother device serves (docs/PROTOCOL.md).
constexpr int kDefaultScanPort = 54921;

// Per-connection state. Allocated in ICD_ScannerOpenTCPIPDevice, hung off
// ScannerObjectInfo::privateData (a vendor field), freed in CloseDevice /
// Cleanup. Holds where the device is (parsed from the host's param dict) and
// the current scanner session, so the session callbacks can log coherently.
struct DeviceContext {
  std::string ipAddress;
  std::string bonjourName;
  int port = kDefaultScanPort;
  ICAScannerSessionID sessionID = 0;
  bool sessionOpen = false;

  // Task 7 scan state. The host's last SetParameters selection, translated to a
  // brscan::Params, is stored here so ICD_ScannerStart can run RunScan with it.
  brscan::Params params;  // Defaults (kColor/flatbed/300) until SetParameters.

  // The scan runs on a background thread so it never blocks icdd's callback
  // thread (PLAN-2-DESIGN.md decision D). Only the device object owns this
  // DeviceContext, so the thread is always joined before the context is freed
  // (StopScan, called from CloseSession / CloseDevice / Cleanup) -- the thread
  // can therefore hold a raw ctx pointer for its whole life without a
  // use-after-free.
  std::thread scanThread;
  std::atomic<bool> cancelRequested{false};

  // Cancel handshake: the worker publishes its live transport here (under
  // scanMutex) so a host cancel can close the socket out from under a blocked
  // RunScan read, which surfaces as Status::kTimeout/kIoError and unwinds the
  // worker cleanly. The transport object itself is owned by the worker's stack
  // frame and outlives every guarded access (the worker clears this pointer
  // under the lock before the transport is destroyed).
  std::mutex scanMutex;
  brscan::TcpTransport* activeTransport = nullptr;
};

DeviceContext* ContextOf(const ScannerObjectInfo* info) {
  return info ? reinterpret_cast<DeviceContext*>(info->privateData) : nullptr;
}

// Reads a CFString value from the host's TCP/IP param dict into `out`.
void CopyStringParam(CFDictionaryRef params, CFStringRef key, std::string* out) {
  if (!params || !out) return;
  const void* value = CFDictionaryGetValue(params, key);
  if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
    char buf[256] = {0};
    if (CFStringGetCString((CFStringRef)value, buf, sizeof(buf),
                           kCFStringEncodingUTF8)) {
      *out = buf;
    }
  }
}

// Reads an integer-valued param (CFNumber, or a CFString holding digits).
void CopyIntParam(CFDictionaryRef params, CFStringRef key, int* out) {
  if (!params || !out) return;
  const void* value = CFDictionaryGetValue(params, key);
  if (!value) return;
  if (CFGetTypeID(value) == CFNumberGetTypeID()) {
    int n = 0;
    if (CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &n)) *out = n;
  } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
    *out = (int)CFStringGetIntValue((CFStringRef)value);
  }
}

// Reads a boolean-valued param. Sets `*out` and returns true only if the key is
// present (CFBoolean, or a CFNumber treated as nonzero == true).
bool CopyBoolParam(CFDictionaryRef params, CFStringRef key, bool* out) {
  if (!params || !out) return false;
  const void* value = CFDictionaryGetValue(params, key);
  if (!value) return false;
  if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
    *out = CFBooleanGetValue((CFBooleanRef)value);
    return true;
  }
  if (CFGetTypeID(value) == CFNumberGetTypeID()) {
    int n = 0;
    if (CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &n)) {
      *out = (n != 0);
      return true;
    }
  }
  return false;
}

// Returns the CFDictionary value for `key`, or null if absent / not a dict.
CFDictionaryRef GetSubDict(CFDictionaryRef dict, CFStringRef key) {
  if (!dict) return nullptr;
  const void* value = CFDictionaryGetValue(dict, key);
  if (value && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
    return (CFDictionaryRef)value;
  }
  return nullptr;
}

// Reads the integer payload of a nested TWAIN-style capability entry. The host
// echoes each ICAP_* selection as a sub-dict {type, value[, current]} (Plan 2
// Task 11 live trace); this prefers `value` and falls back to `current`. If the
// key is not a sub-dict it is read directly as an int at `parent` (a flat
// encoding fallback), so the parse is robust to either shape. Sets *has and *out
// only when a numeric payload is found; leaves them untouched otherwise.
void ReadIcapEntry(CFDictionaryRef parent, CFStringRef key, bool* has,
                   int* out) {
  if (!parent || !has || !out) return;
  int probe = INT32_MIN;
  CFDictionaryRef entry = GetSubDict(parent, key);
  if (entry) {
    CopyIntParam(entry, CFSTR("value"), &probe);
    if (probe == INT32_MIN) CopyIntParam(entry, CFSTR("current"), &probe);
  } else {
    CopyIntParam(parent, key, &probe);
  }
  if (probe != INT32_MIN) {
    *has = true;
    *out = probe;
  }
}

// Reads the host's SetParameters CFDictionary into a framework-free ScanRequest.
// Plan 2 Task 11 live trace: the host nests the ICAP_* selection inside ONE
// top-level `userScanArea` dictionary, each entry a TWAIN {type, value[,current]}
// sub-dict (ICAP_XRESOLUTION / ICAP_YRESOLUTION / ICAP_PIXELTYPE / ICAP_BITDEPTH
// / ICAP_UNITS, plus the scan-rectangle offset/extent). Older builds put keys at
// the top level, and Task 10 introduced a `device` wrapper on the capability
// schema, so this prefers the nested userScanArea, falls back to the top-level
// dict, and also probes a `device` wrapper. The CoreFoundation traversal lives
// here; the pure integers -> ScanRequest mapping is scan_translate's
// ScanRequestFromIcap (unit-tested), and the userScanArea -> corners conversion
// is CornersFromUserScanArea (also unit-tested).
brscan::ica::ScanRequest ReadScanRequest(CFDictionaryRef dict) {
  CFDictionaryRef area = GetSubDict(dict, CFSTR("userScanArea"));
  if (!area) {
    CFDictionaryRef device = GetSubDict(dict, CFSTR("device"));
    if (device) area = GetSubDict(device, CFSTR("userScanArea"));
  }
  CFDictionaryRef src = area ? area : dict;

  // Full dump of the parameter source (the Task-8 log truncated before the
  // scan-area offset/extent). This is what confirms the real area key names for
  // a follow-up, so the truncation is deliberately removed here.
  os_log(Log(),
         "ReadScanRequest: userScanArea present=%d; FULL source dict: %{public}@",
         area != nullptr, (__bridge NSDictionary*)src);

  brscan::ica::IcapScanSelection sel;
  ReadIcapEntry(src, CFSTR("ICAP_XRESOLUTION"), &sel.has_x_resolution,
                &sel.x_resolution);
  ReadIcapEntry(src, CFSTR("ICAP_YRESOLUTION"), &sel.has_y_resolution,
                &sel.y_resolution);
  ReadIcapEntry(src, CFSTR("ICAP_PIXELTYPE"), &sel.has_pixel_type,
                &sel.pixel_type);
  ReadIcapEntry(src, CFSTR("ICAP_BITDEPTH"), &sel.has_bit_depth,
                &sel.bit_depth);
  ReadIcapEntry(src, CFSTR("ICAP_UNITS"), &sel.has_units, &sel.units);

  // Functional unit / duplex: prefer the nested source, fall back to top level.
  ReadIcapEntry(src, CFSTR("selectedFunctionalUnitType"),
                &sel.has_functional_unit, &sel.functional_unit);
  if (!sel.has_functional_unit) {
    ReadIcapEntry(dict, CFSTR("selectedFunctionalUnitType"),
                  &sel.has_functional_unit, &sel.functional_unit);
  }
  if (!CopyBoolParam(src, CFSTR("duplex"), &sel.duplex)) {
    CopyBoolParam(dict, CFSTR("duplex"), &sel.duplex);
  }

  // Scan rectangle offset/extent. The exact key spellings are device-in-the-loop
  // (the live log truncated before them); probe the likeliest names. The full
  // dump above is the ground truth that confirms them for a follow-up.
  ReadIcapEntry(src, CFSTR("offsetX"), &sel.has_offset_x, &sel.offset_x);
  ReadIcapEntry(src, CFSTR("offsetY"), &sel.has_offset_y, &sel.offset_y);
  ReadIcapEntry(src, CFSTR("width"), &sel.has_width, &sel.width);
  ReadIcapEntry(src, CFSTR("height"), &sel.has_height, &sel.height);

  brscan::ica::ScanRequest req = brscan::ica::ScanRequestFromIcap(sel);

  os_log(Log(),
         "ReadScanRequest: parsed xres=%d(has=%d) yres=%d pixeltype=%d(has=%d) "
         "bitdepth=%d units=%d funit=%d(has=%d) area=%d(%d,%d,%d,%d)",
         sel.x_resolution, sel.has_x_resolution, sel.y_resolution,
         sel.pixel_type, sel.has_pixel_type, sel.bit_depth, sel.units,
         sel.functional_unit, sel.has_functional_unit, req.has_area,
         req.area_x0, req.area_y0, req.area_x1, req.area_y1);
  return req;
}

// Human-readable names for os_log tracing of the translated params.
const char* ModeName(brscan::ScanMode mode) {
  switch (mode) {
    case brscan::ScanMode::kColor:
      return "color";
    case brscan::ScanMode::kGray:
      return "gray";
    case brscan::ScanMode::kBlackWhite:
      return "blackwhite";
    case brscan::ScanMode::kErrorDiffusion:
      return "errdif";
    case brscan::ScanMode::kTrueGray:
      return "truegray";
  }
  return "?";
}

const char* SourceName(brscan::Source source) {
  return source == brscan::Source::kAdf ? "adf" : "flatbed";
}

const char* PixelFormatName(brscan::PixelFormat format) {
  switch (format) {
    case brscan::PixelFormat::kRgb:
      return "rgb";
    case brscan::PixelFormat::kGray:
      return "gray";
    case brscan::PixelFormat::kBitonal:
      return "bitonal";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Device open / close.
//
// ICD_ScannerOpenTCPIPDevice is the connection step behind the client's
// requestOpenSession: icdd resolves the matched _scanner._tcp service, hands us
// its address in `params`, and expects us to fill `objectInfo` for the device
// object it allocated (icaObject is Apple's; the vendor fills icaObjectInfo /
// name / uniqueID / privateData). Leaving objectInfo zeroed is what produced
// -21345; populating it is the fix.

ICAError OpenTCPIPDevice(CFDictionaryRef params, ScannerObjectInfo* objectInfo) {
  os_log(Log(), "callback: ICD_ScannerOpenTCPIPDevice (params=%ld keys)",
         params ? CFDictionaryGetCount(params) : -1);
  if (objectInfo == nullptr) {
    os_log_error(Log(), "OpenTCPIPDevice: null objectInfo");
    return kICADeviceInvalidParamErr;
  }

  auto* ctx = new (std::nothrow) DeviceContext();
  if (ctx == nullptr) return kICADeviceMemoryAllocationErr;

  // The host's TCP/IP param dict carries the resolved endpoint under the public
  // kICAIP* / kICABonjour* keys (ICAApplication.h). Any subset may be present.
  CopyStringParam(params, kICAIPAddressKey, &ctx->ipAddress);
  CopyStringParam(params, kICAIPNameKey, &ctx->bonjourName);
  if (ctx->bonjourName.empty()) {
    CopyStringParam(params, kICABonjourServiceNameKey, &ctx->bonjourName);
  }
  CopyIntParam(params, kICAIPPortKey, &ctx->port);
  if (ctx->port <= 0) ctx->port = kDefaultScanPort;

  os_log(Log(),
         "OpenTCPIPDevice: endpoint ip='%{public}s' name='%{public}s' port=%d",
         ctx->ipAddress.c_str(), ctx->bonjourName.c_str(), ctx->port);

  // Fill the vendor fields of the device object. Best-effort clean-room mapping
  // (confirm against a live trace): a scanner device object is objectType
  // kICADevice with objectSubtype kICADeviceScanner (ICAApplication.h).
  objectInfo->icaObjectInfo.objectType = kICADevice;
  objectInfo->icaObjectInfo.objectSubtype = kICADeviceScanner;

  const std::string& displayName =
      ctx->bonjourName.empty() ? std::string(kSyntheticDeviceName)
                               : ctx->bonjourName;
  std::memset(objectInfo->name, 0, sizeof(objectInfo->name));
  std::strncpy(reinterpret_cast<char*>(objectInfo->name), displayName.c_str(),
               sizeof(objectInfo->name) - 1);

  // A stable non-zero id derived from the name so re-opens are consistent.
  UInt32 uid = 2166136261u;  // FNV-1a basis; identity only, not security.
  for (char c : displayName) {
    uid = (uid ^ static_cast<unsigned char>(c)) * 16777619u;
  }
  objectInfo->uniqueID = uid;
  objectInfo->flags = 0;
  objectInfo->privateData = reinterpret_cast<Ptr>(ctx);

  os_log(Log(),
         "OpenTCPIPDevice: device object filled name='%{public}s' uid=0x%08x "
         "-> returning noErr (connection should open, no -21345)",
         displayName.c_str(), uid);
  return noErr;
}

// Stops any in-flight scan and joins the worker thread. Called before the
// DeviceContext is freed and on a host cancel (CloseSession). Sets the cancel
// flag, then closes the live socket out from under a blocked RunScan read so it
// returns promptly (Status::kTimeout/kIoError), then joins. Idempotent and safe
// to call when no scan is running: the join on a non-joinable thread is skipped
// and the transport pointer is null. Because the worker holds a raw ctx pointer,
// this join MUST complete before the context is deleted -- otherwise the worker
// would touch freed memory.
void StopScan(DeviceContext* ctx) {
  if (ctx == nullptr) return;
  ctx->cancelRequested.store(true);
  {
    std::lock_guard<std::mutex> lock(ctx->scanMutex);
    if (ctx->activeTransport != nullptr) ctx->activeTransport->Disconnect();
  }
  if (ctx->scanThread.joinable()) ctx->scanThread.join();
}

ICAError CloseDevice(ScannerObjectInfo* objectInfo) {
  os_log(Log(), "callback: ICD_ScannerCloseDevice");
  DeviceContext* ctx = ContextOf(objectInfo);
  if (ctx) {
    StopScan(ctx);  // Join the worker before freeing the context it points at.
    delete ctx;
    objectInfo->privateData = nullptr;
  }
  return noErr;
}

ICAError Cleanup(ScannerObjectInfo* objectInfo) {
  os_log(Log(), "callback: ICD_ScannerCleanup");
  // Defensive: free any context still attached (CloseDevice normally does).
  DeviceContext* ctx = ContextOf(objectInfo);
  if (ctx) {
    StopScan(ctx);  // Same ordering guarantee as CloseDevice.
    delete ctx;
    objectInfo->privateData = nullptr;
  }
  return noErr;
}

ICAError PeriodicTask(ScannerObjectInfo* /*objectInfo*/) {
  // Fires repeatedly while the device is open; log once so it can never flood.
  LOG_ONCE("callback: ICD_ScannerPeriodicTask (fires repeatedly; logged once)");
  return noErr;
}

// Object-tree enumeration. icdd builds the host-visible object tree by calling
// this for index 0, 1, 2, … under a parent and stopping at kICAIndexOutOfRangeErr
// (the callback's `/* index is zero based */` contract in ICD_ScannerCalls.h).
//
// The Task-2 build returned kICAIndexOutOfRangeErr for EVERY index — i.e. it
// reported the device object as having zero children. That did not terminate
// icdd's walk: it re-drove ICD_ScannerGetObjectInfo in a tight, unbounded loop
// (hundreds of calls a millisecond) and never reached a ready state, because a
// scanner device object is a container icdd expects to hold its one scan object,
// and an empty container is treated as "not built yet → walk again". (The
// earlier load-spike had the mirror-image bug: it returned noErr for every
// index, i.e. an endless supply of children — an infinite walk the other way.)
//
// The fix is a correct, finite tree: the device object (kICADevice) reports
// exactly ONE child at index 0 — the scan object the host binds the scan panel
// to — and kICAIndexOutOfRangeErr at index >= 1. That scan object is a leaf: any
// parent that is not the device object (the scan object itself, or none) reports
// no children, so the walk terminates at a two-node tree and can never recurse.
//
// CLEAN-ROOM / UNVERIFIED (see docs report): the requirement that the device
// expose exactly one child (rather than zero) is the diagnosis most consistent
// with BOTH observed loops above; it is inferred from the public callback
// contract and the device's black-box behaviour, not confirmed against a live
// icdd trace. If a re-test shows the walk still loops OR a phantom object
// appears, the alternative to try is zero children delivered as a leaf device
// object (mark the device non-container in ICD_ScannerOpenTCPIPDevice).
ICAError GetObjectInfo(const ScannerObjectInfo* parentInfo, UInt32 index,
                       ScannerObjectInfo* newInfo) {
  // Only the device object holds a child; every other parent is a leaf. This is
  // what bounds the tree and prevents any recursion.
  const bool parentIsDevice =
      parentInfo != nullptr &&
      parentInfo->icaObjectInfo.objectType == kICADevice;

  if (!parentIsDevice) {
    LOG_ONCE("callback: ICD_ScannerGetObjectInfo (leaf parent) -> end-of-list");
    return kICAIndexOutOfRangeErr;
  }

  if (index > 0) {
    LOG_ONCE("callback: ICD_ScannerGetObjectInfo device child list ended "
             "(one child)");
    return kICAIndexOutOfRangeErr;
  }

  if (newInfo == nullptr) return kICADeviceInvalidParamErr;

  // The single scan object. A leaf (objectType != kICADevice, so its own
  // enumeration returns end-of-list above), carrying no data yet — a real page
  // object is created when a scan runs (PLAN-2 task 7). privateData stays null:
  // only the device object owns a DeviceContext, so CloseDevice / Cleanup on
  // this object are safe no-ops and never double-free the device context.
  newInfo->icaObjectInfo.objectType = kICAFile;
  newInfo->icaObjectInfo.objectSubtype = kICAFileImage;
  newInfo->uniqueID = parentInfo->uniqueID ^ 0x5343414eu;  // 'SCAN'; stable.
  newInfo->thumbnailSize = 0;
  newInfo->dataSize = 0;
  newInfo->dataWidth = 0;
  newInfo->dataHeight = 0;
  newInfo->flags = 0;
  newInfo->privateData = nullptr;
  std::memset(newInfo->name, 0, sizeof(newInfo->name));
  std::strncpy(reinterpret_cast<char*>(newInfo->name), "Scan",
               sizeof(newInfo->name) - 1);

  LOG_ONCE("callback: ICD_ScannerGetObjectInfo -> scan object (index 0), "
           "child list terminates at index 1");
  return noErr;
}

// Standard object properties: defer to the framework helper, which supplies the
// common property set from the object info we filled at open.
ICAError GetPropertyData(const ScannerObjectInfo* objectInfo, void* pb) {
  // icdd polls property data repeatedly while the panel is open; log once.
  LOG_ONCE("callback: ICD_ScannerGetPropertyData");
  return ICDScannerGetStandardPropertyData(objectInfo, pb);
}

ICAError SetPropertyData(const ScannerObjectInfo* /*objectInfo*/,
                         const void* /*pb*/) {
  os_log(Log(), "callback: ICD_ScannerSetPropertyData");
  return noErr;
}

// ---------------------------------------------------------------------------
// Session lifecycle.

ICAError OpenSession(const ScannerObjectInfo* deviceObjectInfo,
                     ICD_ScannerOpenSessionPB* pb) {
  os_log(Log(), "callback: ICD_ScannerOpenSession");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (ctx) {
    ctx->sessionID = pb->sessionID;
    ctx->sessionOpen = true;
  }
  pb->header.err = noErr;
  os_log(Log(), "OpenSession: sessionID=%u opened (no -21345)", pb->sessionID);
  return noErr;
}

ICAError CloseSession(const ScannerObjectInfo* deviceObjectInfo,
                      ICD_ScannerCloseSessionPB* pb) {
  os_log(Log(), "callback: ICD_ScannerCloseSession");
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (ctx) {
    // A session close mid-scan is the host's cancel path (there is no dedicated
    // ICD_ScannerCancel callback in the SDK table): stop and join the worker
    // here. After a completed scan this simply joins the finished thread.
    StopScan(ctx);
    ctx->sessionOpen = false;
  }
  if (pb) pb->header.err = noErr;
  return noErr;
}

ICAError Initialize(const ScannerObjectInfo* /*deviceObjectInfo*/,
                    ICD_ScannerInitializePB* pb) {
  os_log(Log(), "callback: ICD_ScannerInitialize");
  if (pb) pb->header.err = noErr;
  return noErr;
}

// ---------------------------------------------------------------------------
// Parameters / status / start.

ICAError GetParameters(const ScannerObjectInfo* /*deviceObjectInfo*/,
                       ICD_ScannerGetParametersPB* pb) {
  os_log(Log(), "callback: ICD_ScannerGetParameters");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  if (pb->theDict != nullptr) {
    // DIAGNOSTIC (Task 8): dump the INCOMING dict BEFORE we populate it. This
    // reveals whether icdd pre-seeds the capability keys it expects (in which
    // case we should merge into / mirror its shape rather than invent one). An
    // empty incoming dict means the module alone dictates the schema.
    os_log(Log(),
           "GetParameters: INCOMING theDict (%ld keys) BEFORE populate: %{public}@",
           CFDictionaryGetCount(pb->theDict),
           (__bridge NSDictionary*)pb->theDict);
    brscan::ica::BuildScannerParameters(pb->theDict);
    os_log(Log(), "GetParameters: described %ld parameter keys",
           CFDictionaryGetCount(pb->theDict));
  } else {
    os_log_error(Log(), "GetParameters: theDict is null");
  }
  pb->header.err = noErr;
  return noErr;
}

ICAError SetParameters(const ScannerObjectInfo* deviceObjectInfo,
                       ICD_ScannerSetParametersPB* pb) {
  os_log(Log(), "callback: ICD_ScannerSetParameters");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (pb->theDict) {
    os_log(Log(), "SetParameters: host set %ld keys: %{public}@",
           CFDictionaryGetCount(pb->theDict),
           (__bridge NSDictionary*)pb->theDict);

    // Translate the host's selection to a brscan::Params and stash it on the
    // device context for ICD_ScannerStart to scan with.
    brscan::ica::ScanRequest req = ReadScanRequest(pb->theDict);
    brscan::ica::ScanLimits limits;  // default max_dpi = highest offer (600).
    brscan::Params params = brscan::ica::TranslateScanParams(req, limits);
    if (ctx) ctx->params = params;

    os_log(Log(),
           "SetParameters: translated -> mode=%{public}s dpi=%d source=%{public}s "
           "duplex=%d area=(%d,%d,%d,%d) brightness=%d contrast=%d",
           ModeName(params.mode), params.x_dpi, SourceName(params.source),
           params.duplex, params.area.x0, params.area.y0, params.area.x1,
           params.area.y1, params.brightness, params.contrast);
  }
  pb->header.err = noErr;
  return noErr;
}

ICAError Status(const ScannerObjectInfo* deviceObjectInfo,
                ICD_ScannerStatusPB* pb) {
  // icdd polls status; log once so a poll can never spew.
  LOG_ONCE("callback: ICD_ScannerStatus");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  // 0 = ready/available. We do not (yet) probe the live device here; a real
  // reachability/busy check belongs with the scan-execution task.
  pb->status = 0;
  pb->header.err = noErr;
  LOG_ONCE("Status: reporting 0 (ready), sessionOpen=%d",
           ctx ? ctx->sessionOpen : 0);
  return noErr;
}

// ---------------------------------------------------------------------------
// Scan execution and data hand-back (Task 7).
//
// HAND-BACK MECHANISM (chosen from the public SDK headers): in-memory band
// hand-back via the notification dictionary API -- design decision G option (b),
// NOT a file/ReadFileData pull. The evidence, all public:
//   - ICD_ScannerStartPB (ICD_ScannerCalls.h) carries NO output destination,
//     file name, folder, transfer mode, or data type -- it is purely a trigger
//     (header/object/objectInfo/connectionID/sessionID). So the pixels are not
//     handed back through Start's parameter block; they are pushed afterwards.
//   - ICDAddBandInfoToNotificationDictionary(dict, width, height, bitsPerPixel,
//     bitsPerComponent, numComponents, endianness, pixelDataType, bytesPerRow,
//     dataStartRow, dataNumberOfRows, dataSize, dataBuffer) (ICADevices.h) is
//     the band packer -- its arguments are exactly the descriptor
//     brscan::ica::DescribeBuffer computes.
//   - ICASendNotificationPB { header; notificationDictionary; replyCode; }
//     (ICAApplication.h) + ICDSendNotification / ICDSendNotificationAndWaitForReply
//     (ICADevices.h) push it to icdd.
//   - kICANotificationTypeScanProgressStatus (band payload),
//     kICANotificationTypeScannerPageDone / kICANotificationTypeScannerScanDone,
//     keyed by kICANotificationTypeKey, each referencing the scanned object under
//     kICANotificationICAObjectKey (ICAApplication.h) -- see the orchestration
//     note below for which notification carries the pixels.
// All of these symbols are exported by ICADevices.tbd, so the module links.
//
// HAND-BACK ORCHESTRATION (Task 11, corrected against the ICADevices reference
// modules -- interface facts only, clean-room):
//   - The pixels are delivered INLINE as one full-height band in a
//     kICANotificationTypeScanProgressStatus notification (via
//     ICDAddBandInfoToNotificationDictionary), NOT in the page-done notification.
//     Confirmed by Apple's VirtualScanner (Sources/VirtualScanner.m): its
//     "scan data" transfer mode packs the band into a ScanProgressStatus and
//     sends it with ICDSendNotificationAndWaitForReply (the replyCode carries a
//     user cancel); page-done in that mode carries no data.
//   - EVERY scanner notification references the device/scanner object's
//     framework-assigned ICAObject (ScannerObjectInfo::icaObject, which for a
//     scan is ICD_ScannerStartPB::object) under kICANotificationICAObjectKey.
//     VirtualScanner sets this on every ScanProgressStatus / ScannerPageDone /
//     ScannerScanDone; it does NOT use kICANotificationDeviceICAObjectKey here.
//   - kICANotificationTypeScannerPageDone then signals the page is complete, and
//     a final kICANotificationTypeScannerScanDone ends the job -- both carrying
//     the same device ICAObject and no pixel payload.
//
// WHY THE TASK-7 BUILD SHOWED NOTHING (two defects, both fixed here):
//   1. It sent the band inside a ScannerPageDone, not a ScanProgressStatus, so
//      the host treated the page as complete-with-no-data and dropped the pixels.
//   2. It never set kICANotificationICAObjectKey: it tried to mint a per-page
//      object with ICDNewObject (ICADevice.h), which returns unimpErr (-4) in the
//      scanner-module runtime -- ICDNewObject is the legacy generic object API
//      and is not serviced for scanner modules. The scanner-specific creator is
//      ICDScannerNewObjectInfoCreated (ICD_ScannerCalls.h), used by VirtualScanner
//      ONLY for its file-based network-transfer mode (paired with a
//      kICANotificationTypeObjectAdded and a kICANotificationScannerDocumentNameKey
//      file path), NOT for the in-memory band path this module uses. So no object
//      is minted here; the band path references the existing device ICAObject.
//
// CLEAN-ROOM: reimplemented from the ICADevices public headers and the observed
// key/notification-type usage; no reference source copied. If a live re-test
// shows the host still ignores the bands, the fallback is VirtualScanner's
// file-based mode: write each page to a temp file, mint an object with
// ICDScannerNewObjectInfoCreated, and reference the file via
// kICANotificationScannerDocumentNameKey (see the task report).

// Sets the type + the device/scanner ICAObject (under kICANotificationICAObjectKey,
// the key every scanner notification carries) and pushes the dictionary to the
// host. `waitForReply` uses ICDSendNotificationAndWaitForReply (so the band
// buffer stays alive until icdd has copied it, and a user cancel surfaces in
// replyCode) for the data-bearing ScanProgressStatus; the page/scan-done signals
// are fire-and-forget. Returns the framework's ICAError.
ICAError SendScannerNotification(CFMutableDictionaryRef dict,
                                 ICAObject icaObject, CFStringRef type,
                                 bool waitForReply) {
  if (dict == nullptr) return kICADeviceInvalidParamErr;
  CFDictionarySetValue(dict, kICANotificationTypeKey, type);
  CFNumberRef objNum =
      CFNumberCreate(nullptr, kCFNumberSInt32Type, &icaObject);
  if (objNum) {
    CFDictionarySetValue(dict, kICANotificationICAObjectKey, objNum);
    CFRelease(objNum);
  }
  ICASendNotificationPB pb = {};
  pb.notificationDictionary = dict;
  return waitForReply ? ICDSendNotificationAndWaitForReply(&pb)
                      : ICDSendNotification(&pb);
}

// Hands one decoded page back as a single full-height band, then signals the page
// complete. `bytes` must point at `byteCount` host-ready bytes: interleaved
// 24-bit RGB (post-DecodeJpeg) for kRgb, raw 8-bit gray for kGray, packed 1-bpp
// for kBitonal. `icaObject` is the device/scanner object (ICD_ScannerStartPB::
// object). Returns true if the band was accepted and both notifications sent.
bool PostPage(ICAObject icaObject, brscan::PixelFormat format,
              const uint8_t* bytes, size_t byteCount, int width, int height,
              int pageIndex) {
  std::optional<brscan::ica::BufferDescriptor> d =
      brscan::ica::DescribeBuffer(format, width, height);
  if (!d) {
    os_log_error(Log(), "PostPage[%d]: invalid geometry %dx%d", pageIndex,
                 width, height);
    return false;
  }

  // Map the framework-free descriptor onto the band-info arguments. pixelDataType
  // is the ImageCaptureCore client enum (0=BW, 1=Gray, 2=RGB).
  UInt32 pixelDataType = 2;
  UInt32 bitsPerComponent = 8;
  switch (format) {
    case brscan::PixelFormat::kRgb:
      pixelDataType = 2;
      bitsPerComponent = 8;
      break;
    case brscan::PixelFormat::kGray:
      pixelDataType = 1;
      bitsPerComponent = 8;
      break;
    case brscan::PixelFormat::kBitonal:
      pixelDataType = 0;
      bitsPerComponent = 1;
      break;
  }

  // 1) Deliver the pixels: one full-height band in a ScanProgressStatus, waiting
  //    for the reply so `bytes` outlives the copy and a cancel is observed.
  CFMutableDictionaryRef bandDict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (bandDict == nullptr) return false;

  ICAError addErr = ICDAddBandInfoToNotificationDictionary(
      bandDict, static_cast<UInt32>(width), static_cast<UInt32>(height),
      static_cast<UInt32>(d->bits_per_pixel), bitsPerComponent,
      static_cast<UInt32>(d->samples_per_pixel), /*endianness=*/0,
      pixelDataType, static_cast<UInt32>(d->bytes_per_row),
      /*dataStartRow=*/0, /*dataNumberOfRows=*/static_cast<UInt32>(height),
      static_cast<UInt32>(byteCount), const_cast<uint8_t*>(bytes));

  ICAError bandErr = SendScannerNotification(
      bandDict, icaObject, kICANotificationTypeScanProgressStatus,
      /*waitForReply=*/true);
  CFRelease(bandDict);

  // 2) Signal the page complete (no payload).
  CFMutableDictionaryRef pageDict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  ICAError pageErr = kICADeviceInvalidParamErr;
  if (pageDict) {
    pageErr = SendScannerNotification(pageDict, icaObject,
                                      kICANotificationTypeScannerPageDone,
                                      /*waitForReply=*/false);
    CFRelease(pageDict);
  }

  os_log(Log(),
         "PostPage[%d]: %dx%d bpp=%d spp=%lld stride=%lld bytes=%zu "
         "icaObject=0x%08x addBandInfo=%d sendProgressBand=%d sendPageDone=%d",
         pageIndex, width, height, d->bits_per_pixel,
         (long long)d->samples_per_pixel, (long long)d->bytes_per_row,
         byteCount, icaObject, addErr, bandErr, pageErr);
  return addErr == noErr && bandErr == noErr && pageErr == noErr;
}

// The background scan worker. Owns its TcpTransport on the stack; publishes it to
// ctx->activeTransport (under scanMutex) only for the cancel path. Runs the
// normal host-initiated RunScan (button_flow == false), then hands each page
// back and finishes with a ScannerScanDone. Holds a raw ctx pointer, kept valid
// by StopScan joining this thread before the context is freed.
void RunScanWorker(DeviceContext* ctx, brscan::Params params,
                   ICAObject deviceObject) {
  os_log(Log(),
         "ScanWorker: begin ip='%{public}s' port=%d mode=%{public}s dpi=%d "
         "source=%{public}s duplex=%d",
         ctx->ipAddress.c_str(), ctx->port, ModeName(params.mode), params.x_dpi,
         SourceName(params.source), params.duplex);

  brscan::TcpTransport transport(ctx->ipAddress,
                                 static_cast<uint16_t>(ctx->port));
  {
    std::lock_guard<std::mutex> lock(ctx->scanMutex);
    ctx->activeTransport = &transport;
  }

  ICAError finalErr = noErr;
  // Bounded connect retry (Task 11): the Brother device allows a single scan
  // connection, so a lingering socket from a prior session can lose the first
  // race (observed live: connect -> 1, then success on retry). Try up to
  // kConnectAttempts times, ~kConnectBackoff apart, bailing early on a cancel.
  constexpr int kConnectAttempts = 3;
  constexpr auto kConnectBackoff = std::chrono::milliseconds(500);
  brscan::Status connectStatus = brscan::Status::kIoError;
  for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
    if (ctx->cancelRequested.load()) break;
    connectStatus = transport.Connect();
    os_log(Log(), "ScanWorker: transport connect attempt %d/%d -> %d", attempt,
           kConnectAttempts, (int)connectStatus);
    if (connectStatus == brscan::Status::kOk) break;
    if (attempt < kConnectAttempts && !ctx->cancelRequested.load()) {
      std::this_thread::sleep_for(kConnectBackoff);
    }
  }

  if (connectStatus != brscan::Status::kOk) {
    finalErr = kICADeviceInternalErr;
  } else {
    std::vector<brscan::ScanResult> pages;
    const brscan::Status scanStatus =
        brscan::RunScan(transport, params, &pages);
    os_log(Log(), "ScanWorker: RunScan -> status=%d pages=%zu",
           (int)scanStatus, pages.size());

    if (scanStatus != brscan::Status::kOk) {
      // A mid-scan cancel surfaces as kTimeout once StopScan closes the socket;
      // treat a requested cancel as a clean stop, anything else as an error.
      finalErr = ctx->cancelRequested.load() ? noErr : kICADeviceInternalErr;
    } else {
      int idx = 0;
      for (const brscan::ScanResult& page : pages) {
        if (ctx->cancelRequested.load()) {
          os_log(Log(), "ScanWorker: cancel observed, stopping at page %d", idx);
          break;
        }
        os_log(Log(), "ScanWorker: page %d %dx%d format=%{public}s payload=%zu",
               idx, page.width, page.height, PixelFormatName(page.format),
               page.data.size());

        if (page.format == brscan::PixelFormat::kRgb) {
          // Color pages arrive as baseline JPEG; decode to interleaved RGB.
          brscan::Image img;
          const brscan::Status dec =
              brscan::DecodeJpeg(page.data.data(), page.data.size(), &img);
          if (dec != brscan::Status::kOk) {
            os_log_error(Log(), "ScanWorker: page %d JPEG decode failed (%d)",
                         idx, (int)dec);
            finalErr = kICADeviceInternalErr;
          } else {
            PostPage(deviceObject, brscan::PixelFormat::kRgb, img.pixels.data(),
                     img.pixels.size(), img.width, img.height, idx);
          }
        } else {
          // Gray / bitonal already carry host-ready bytes.
          PostPage(deviceObject, page.format, page.data.data(),
                   page.data.size(), page.width, page.height, idx);
        }
        ++idx;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(ctx->scanMutex);
    ctx->activeTransport = nullptr;
  }
  transport.Disconnect();

  // Completion: always tell the host the scan is done, carrying an error code
  // when one occurred so the scan UI does not hang waiting for more pages.
  CFMutableDictionaryRef done = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (done) {
    if (finalErr != noErr) {
      CFNumberRef errNum =
          CFNumberCreate(nullptr, kCFNumberSInt32Type, &finalErr);
      if (errNum) {
        CFDictionarySetValue(done, kICAErrorKey, errNum);
        CFRelease(errNum);
      }
    }
    ICAError sendErr = SendScannerNotification(
        done, deviceObject, kICANotificationTypeScannerScanDone,
        /*waitForReply=*/false);
    CFRelease(done);
    os_log(Log(), "ScanWorker: ScannerScanDone sent err=%d (finalErr=%d)",
           sendErr, finalErr);
  }
  os_log(Log(), "ScanWorker: end");
}

ICAError Start(const ScannerObjectInfo* deviceObjectInfo,
               ICD_ScannerStartPB* pb) {
  os_log(Log(), "callback: ICD_ScannerStart");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (ctx == nullptr) {
    os_log_error(Log(), "Start: no device context");
    pb->header.err = kICADeviceInternalErr;
    return kICADeviceInternalErr;
  }
  if (ctx->ipAddress.empty()) {
    os_log_error(Log(), "Start: no device endpoint (empty ipAddress)");
    pb->header.err = kICADeviceInternalErr;
    return kICADeviceInternalErr;
  }

  // Reap any prior worker (e.g. a scan that already finished) before starting a
  // new one, and clear the cancel flag for this run.
  if (ctx->scanThread.joinable()) ctx->scanThread.join();
  ctx->cancelRequested.store(false);

  const brscan::Params params = ctx->params;
  const ICAObject deviceObject = pb->object;
  os_log(Log(),
         "Start: launching scan worker mode=%{public}s dpi=%d source=%{public}s "
         "duplex=%d area=(%d,%d,%d,%d)",
         ModeName(params.mode), params.x_dpi, SourceName(params.source),
         params.duplex, params.area.x0, params.area.y0, params.area.x1,
         params.area.y1);

  // Run off icdd's callback thread; completion is posted via ScannerScanDone.
  ctx->scanThread = std::thread(RunScanWorker, ctx, params, deviceObject);

  pb->header.err = noErr;
  return noErr;
}

// Registers the callbacks icdd drives to open, describe, and (attempt to) scan
// the device. Entry points we do not implement stay NULL — the framework
// tolerates a partial table (the load spike relied on this).
void RegisterCallbacks() {
  gICDScannerCallbackFunctions.f_ICD_ScannerOpenTCPIPDevice = OpenTCPIPDevice;
  gICDScannerCallbackFunctions.f_ICD_ScannerCloseDevice = CloseDevice;
  gICDScannerCallbackFunctions.f_ICD_ScannerCleanup = Cleanup;
  gICDScannerCallbackFunctions.f_ICD_ScannerPeriodicTask = PeriodicTask;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetObjectInfo = GetObjectInfo;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetPropertyData = GetPropertyData;
  gICDScannerCallbackFunctions.f_ICD_ScannerSetPropertyData = SetPropertyData;
  gICDScannerCallbackFunctions.f_ICD_ScannerOpenSession = OpenSession;
  gICDScannerCallbackFunctions.f_ICD_ScannerCloseSession = CloseSession;
  gICDScannerCallbackFunctions.f_ICD_ScannerInitialize = Initialize;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetParameters = GetParameters;
  gICDScannerCallbackFunctions.f_ICD_ScannerSetParameters = SetParameters;
  gICDScannerCallbackFunctions.f_ICD_ScannerStatus = Status;
  gICDScannerCallbackFunctions.f_ICD_ScannerStart = Start;
}

}  // namespace

int main(int argc, const char* argv[]) {
  os_log(Log(), "main: Brscan ICA module launched (argc=%d)", argc);
  RegisterCallbacks();
  os_log(Log(), "main: callbacks registered, entering ICD_ScannerMain");
  // ICD_ScannerMain runs the module's service loop and does not return in
  // normal operation; the host tears the process down.
  int rc = ICD_ScannerMain(argc, argv);
  os_log(Log(), "main: ICD_ScannerMain returned %d (unexpected)", rc);
  return rc;
}
