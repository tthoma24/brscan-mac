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

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ICADevices/ICADevices.h>
#import <ImageIO/ImageIO.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/log.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <thread>  // std::this_thread::sleep_for for the connect backoff only.
#include <utility>
#include <vector>

#include "brscan/scanner.h"
#include "brscan/transport_tcp.h"
#include "brscan/types.h"
#include "buffer_descriptor.h"
#include "decode_jpeg.h"  // libbrscan private header (on the libbrscan inc dir).
#include "file_transfer.h"
#include "scan_outcome.h"
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

  // The framework-assigned DEVICE object (ScannerObjectInfo::icaObject, marked
  // "Apple" in ICD_ScannerCalls.h). This -- NOT ICD_ScannerStartPB::object -- is
  // the object every scanner notification must reference under
  // kICANotificationICAObjectKey (Task 15): the host keys its scan session on the
  // device object, and posting notifications against Start's `object` left Image
  // Capture waiting forever ("Scanning document" hang) and never rendering the
  // overview. Captured in OpenSession and Start from deviceObjectInfo->icaObject
  // and passed to the synchronous scan (Task 16).
  ICAObject deviceObject = 0;

  // Task 7 scan state. The host's last SetParameters selection, translated to a
  // brscan::Params, is stored here so ICD_ScannerStart can run RunScan with it.
  brscan::Params params;  // Defaults (kColor/flatbed/300) until SetParameters.

  // The ICScannerFunctionalUnitType the host has currently selected (0=flatbed,
  // 3=document feeder). Selecting the feeder makes the host send SetParameters
  // with selectedFunctionalUnitType=3 and then re-call GetParameters; unless
  // GetParameters answers with the SELECTED unit the host re-selects it forever
  // ("Waiting for Scanner"). SetParameters records the choice here and
  // GetParameters advertises it via BuildScannerParameters. Defaults to flatbed.
  int selectedFunctionalUnit = 0;

  // Task 12/13 transfer mode. A FINAL scan carries a destination (a
  // security-scoped folder URL and/or a "document folder" path) plus document
  // name/format/extension keys -> file-based transfer: encode the page and WRITE
  // it there. An overview/preview scan carries none of these -> the in-memory
  // image path. SetParameters detects the mode and STAGES it in these fields;
  // ICD_ScannerStart then consumes them. Because Start runs the whole scan
  // synchronously on icdd's callback thread (Plan 2 Task 16), SetParameters and
  // Start never overlap -- they alternate on the one callback thread -- so these
  // fields need no locking and no per-job snapshot. `securityScopedURL` is a
  // retained CFURLRef (or null): SetParameters releases/replaces it, Start takes
  // ownership for the scan and releases it after the write, and CloseDevice /
  // Cleanup release it if no scan claimed it.
  bool fileTransfer = false;
  brscan::ica::TransferPlan transferPlan;
  std::string documentFolderPath;   // Plain-path fallback ("document folder").
  CFURLRef securityScopedURL = nullptr;  // Owned; resolved scoped folder URL.
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

// Reads a real-valued param (CFNumber, or a CFString holding a number). Sets
// `*out` and returns true only when a numeric payload is found. Used for the
// scan-rectangle offset/extent, which the host reports in ICAP_UNITS -- inches
// carries fractional values (e.g. 8.5), so these must be read as doubles, not
// truncated to ints.
bool CopyDoubleParam(CFDictionaryRef params, CFStringRef key, double* out) {
  if (!params || !out) return false;
  const void* value = CFDictionaryGetValue(params, key);
  if (!value) return false;
  if (CFGetTypeID(value) == CFNumberGetTypeID()) {
    double d = 0.0;
    if (CFNumberGetValue((CFNumberRef)value, kCFNumberDoubleType, &d)) {
      *out = d;
      return true;
    }
  } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
    *out = CFStringGetDoubleValue((CFStringRef)value);
    return true;
  }
  return false;
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

// Real-valued counterpart of ReadIcapEntry for the scan-rectangle offset/extent.
// A working ICA scanner module reports these as plain top-level numbers in
// ICAP_UNITS (offsetX/offsetY/width/height); this reads that flat form first and
// also tolerates a nested {value[, current]} sub-dict. Sets *has and *out only
// when a numeric payload is found; leaves them untouched otherwise.
void ReadIcapDouble(CFDictionaryRef parent, CFStringRef key, bool* has,
                    double* out) {
  if (!parent || !has || !out) return;
  double probe = 0.0;
  if (CopyDoubleParam(parent, key, &probe)) {
    *has = true;
    *out = probe;
    return;
  }
  CFDictionaryRef entry = GetSubDict(parent, key);
  if (entry) {
    if (CopyDoubleParam(entry, CFSTR("value"), &probe) ||
        CopyDoubleParam(entry, CFSTR("current"), &probe)) {
      *has = true;
      *out = probe;
    }
  }
}

// Human-readable name of the signal that decided the scan source, for tracing a
// feeder scan that unexpectedly ran as flatbed (or vice versa).
const char* SourceSignalName(brscan::ica::SourceSignal signal) {
  switch (signal) {
    case brscan::ica::SourceSignal::kNone:
      return "none(flatbed-default)";
    case brscan::ica::SourceSignal::kExplicitUnit:
      return "explicit-unit";
    case brscan::ica::SourceSignal::kFeederEnabled:
      return "CAP_FEEDERENABLED";
    case brscan::ica::SourceSignal::kTrackedUnit:
      return "tracked-unit";
  }
  return "?";
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
// `trackedFunctionalUnit` is the unit the module recorded from an earlier
// unit-switch SetParameters (DeviceContext.selectedFunctionalUnit), or a
// negative value when there is no device context. It is the source fallback used
// only when the request carries neither an explicit unit nor CAP_FEEDERENABLED.
brscan::ica::ScanRequest ReadScanRequest(CFDictionaryRef dict,
                                         int trackedFunctionalUnit) {
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

  // Functional unit: prefer the nested source, fall back to top level.
  ReadIcapEntry(src, CFSTR("selectedFunctionalUnitType"),
                &sel.has_functional_unit, &sel.functional_unit);
  if (!sel.has_functional_unit) {
    ReadIcapEntry(dict, CFSTR("selectedFunctionalUnitType"),
                  &sel.has_functional_unit, &sel.functional_unit);
  }

  // CAP_FEEDERENABLED: the feeder scan request carries this (== 1) but NO
  // functional unit, so it is the signal that decides the source when no unit is
  // present. Prefer the nested source, fall back to top level.
  ReadIcapEntry(src, CFSTR("CAP_FEEDERENABLED"), &sel.has_feeder_enabled,
                &sel.feeder_enabled);
  if (!sel.has_feeder_enabled) {
    ReadIcapEntry(dict, CFSTR("CAP_FEEDERENABLED"), &sel.has_feeder_enabled,
                  &sel.feeder_enabled);
  }

  // The tracked unit is the last source fallback (see ReadScanRequest contract).
  if (trackedFunctionalUnit >= 0) {
    sel.has_tracked_functional_unit = true;
    sel.tracked_functional_unit = trackedFunctionalUnit;
  }

  // Duplex: the legacy plain `duplex` bool plus the TWAIN duplex capability keys
  // (CAP_DUPLEX = duplexer type, CAP_DUPLEXENABLED = enable toggle). The exact
  // key the host echoes for the 2-sided control is not yet observed live, so all
  // three are read and ScanRequestFromIcap treats any non-zero as "on"; the FULL
  // request dump above captures whichever key actually appears for follow-up.
  if (!CopyBoolParam(src, CFSTR("duplex"), &sel.duplex)) {
    CopyBoolParam(dict, CFSTR("duplex"), &sel.duplex);
  }
  ReadIcapEntry(src, CFSTR("CAP_DUPLEX"), &sel.has_cap_duplex, &sel.cap_duplex);
  if (!sel.has_cap_duplex) {
    ReadIcapEntry(dict, CFSTR("CAP_DUPLEX"), &sel.has_cap_duplex,
                  &sel.cap_duplex);
  }
  ReadIcapEntry(src, CFSTR("CAP_DUPLEXENABLED"), &sel.has_cap_duplex_enabled,
                &sel.cap_duplex_enabled);
  if (!sel.has_cap_duplex_enabled) {
    ReadIcapEntry(dict, CFSTR("CAP_DUPLEXENABLED"),
                  &sel.has_cap_duplex_enabled, &sel.cap_duplex_enabled);
  }

  // Scan rectangle offset/extent. The exact key spellings are device-in-the-loop
  // (the live log truncated before them); probe the likeliest names. The full
  // dump above is the ground truth that confirms them for a follow-up.
  ReadIcapDouble(src, CFSTR("offsetX"), &sel.has_offset_x, &sel.offset_x);
  ReadIcapDouble(src, CFSTR("offsetY"), &sel.has_offset_y, &sel.offset_y);
  ReadIcapDouble(src, CFSTR("width"), &sel.has_width, &sel.width);
  ReadIcapDouble(src, CFSTR("height"), &sel.has_height, &sel.height);

  brscan::ica::ScanRequest req = brscan::ica::ScanRequestFromIcap(sel);

  os_log(Log(),
         "ReadScanRequest: parsed xres=%d(has=%d) yres=%d pixeltype=%d(has=%d) "
         "bitdepth=%d units=%d funit=%d(has=%d) feederEnabled=%d(has=%d) "
         "tracked=%d capDuplex=%d(has=%d) capDuplexEnabled=%d(has=%d) "
         "source=%{public}s resolvedFunit=%d duplex=%d area=%d(%d,%d,%d,%d)",
         sel.x_resolution, sel.has_x_resolution, sel.y_resolution,
         sel.pixel_type, sel.has_pixel_type, sel.bit_depth, sel.units,
         sel.functional_unit, sel.has_functional_unit, sel.feeder_enabled,
         sel.has_feeder_enabled, trackedFunctionalUnit, sel.cap_duplex,
         sel.has_cap_duplex, sel.cap_duplex_enabled, sel.has_cap_duplex_enabled,
         SourceSignalName(req.source_signal), req.functional_unit, req.duplex,
         req.has_area, req.area_x0, req.area_y0, req.area_x1, req.area_y1);
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

// Releases the retained security-scoped destination URL staged on the context,
// if any. Safe to call when none is held. Because the scan runs synchronously on
// the callback thread (Task 16), there is no worker to race: SetParameters,
// Start, and Close* all execute on the one thread, so this only ever frees ctx's
// staged +1.
void ReleaseScopedURL(DeviceContext* ctx) {
  if (ctx && ctx->securityScopedURL) {
    CFRelease(ctx->securityScopedURL);
    ctx->securityScopedURL = nullptr;
  }
}

ICAError CloseDevice(ScannerObjectInfo* objectInfo) {
  os_log(Log(), "callback: ICD_ScannerCloseDevice");
  DeviceContext* ctx = ContextOf(objectInfo);
  if (ctx) {
    // No scan worker to join (Task 16): Start runs the scan synchronously, so by
    // the time CloseDevice is called no scan is in flight. Just free the context.
    ReleaseScopedURL(ctx);
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
    ReleaseScopedURL(ctx);
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
    // Capture the framework-assigned device object now that the framework has
    // populated it; scanner notifications reference this (Task 15).
    if (deviceObjectInfo) ctx->deviceObject = deviceObjectInfo->icaObject;
  }
  pb->header.err = noErr;
  os_log(Log(), "OpenSession: sessionID=%u opened deviceObject=0x%08x (no -21345)",
         pb->sessionID, ctx ? ctx->deviceObject : 0);
  return noErr;
}

ICAError CloseSession(const ScannerObjectInfo* deviceObjectInfo,
                      ICD_ScannerCloseSessionPB* pb) {
  os_log(Log(), "callback: ICD_ScannerCloseSession");
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (ctx) {
    // With a synchronous Start (Task 16) there is no worker to stop: a scan has
    // fully completed (ScannerScanDone sent) before Start returns, so the host
    // only ever reaches CloseSession between scans. Host cancel is handled
    // mid-scan inside RunScan via the per-band progress replyCode instead
    // (Task 18b).
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

ICAError GetParameters(const ScannerObjectInfo* deviceObjectInfo,
                       ICD_ScannerGetParametersPB* pb) {
  os_log(Log(), "callback: ICD_ScannerGetParameters");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  // Advertise the currently SELECTED functional unit (default flatbed). This is
  // what stops the feeder-selection loop: after the host selects the feeder via
  // SetParameters, it re-reads parameters here and must see its choice reflected.
  const int selectedUnit = ctx ? ctx->selectedFunctionalUnit : 0;
  if (pb->theDict != nullptr) {
    // DIAGNOSTIC (Task 8): dump the INCOMING dict BEFORE we populate it. This
    // reveals whether icdd pre-seeds the capability keys it expects (in which
    // case we should merge into / mirror its shape rather than invent one). An
    // empty incoming dict means the module alone dictates the schema.
    os_log(Log(),
           "GetParameters: INCOMING theDict (%ld keys) BEFORE populate: %{public}@",
           CFDictionaryGetCount(pb->theDict),
           (__bridge NSDictionary*)pb->theDict);
    brscan::ica::BuildScannerParameters(pb->theDict, selectedUnit);
    os_log(Log(),
           "GetParameters: described %ld parameter keys "
           "(selectedFunctionalUnitType=%d)",
           CFDictionaryGetCount(pb->theDict), selectedUnit);
  } else {
    os_log_error(Log(), "GetParameters: theDict is null");
  }
  pb->header.err = noErr;
  return noErr;
}

// Logs every top-level key of the host's parameter dict on its own line. A
// whole-dictionary %{public}@ is truncated by os_log's message-size limit, which
// hid the file-transfer keys in earlier builds (the Task-11 log cut off before
// them); one key per line reliably reveals the exact spellings, the value
// classes, and the format token for a live re-test.
void LogFullDict(const char* label, CFDictionaryRef dict) {
  if (dict == nullptr) return;
  NSDictionary* d = (__bridge NSDictionary*)dict;
  os_log(Log(), "%{public}s: %lu top-level keys", label,
         (unsigned long)d.count);
  for (id key in d) {
    id value = [d objectForKey:key];
    os_log(Log(), "  %{public}s['%{public}@'] (%{public}@) = %{public}@", label,
           key, [value class], value);
  }
}

// Looks a key up in the host's parameter dict, checking the top level first and
// then the nested `userScanArea` (and a `device` -> `userScanArea` wrapper).
// The document/destination keys have been seen at the top level, but the host
// also nests the scan selection inside userScanArea (Task 11), so probe both.
id LookupParamValue(CFDictionaryRef dict, NSString* key) {
  if (dict == nullptr) return nil;
  NSDictionary* d = (__bridge NSDictionary*)dict;
  id value = [d objectForKey:key];
  if (value != nil) return value;
  id area = [d objectForKey:@"userScanArea"];
  if ([area isKindOfClass:[NSDictionary class]]) {
    value = [(NSDictionary*)area objectForKey:key];
    if (value != nil) return value;
  }
  id device = [d objectForKey:@"device"];
  if ([device isKindOfClass:[NSDictionary class]]) {
    id deviceArea = [(NSDictionary*)device objectForKey:@"userScanArea"];
    if ([deviceArea isKindOfClass:[NSDictionary class]]) {
      value = [(NSDictionary*)deviceArea objectForKey:key];
    }
  }
  return value;
}

// Reads a string-valued parameter (top level or nested), or "" if absent / not
// a string. Unlike CopyStringParam this has no fixed-size cap, so a long
// destination-folder path is read in full.
std::string LookupStringParam(CFDictionaryRef dict, NSString* key) {
  id value = LookupParamValue(dict, key);
  if ([value isKindOfClass:[NSString class]]) {
    const char* c = [(NSString*)value UTF8String];
    return c ? std::string(c) : std::string();
  }
  return std::string();
}

// Unwraps an NSSecurityScopedURLWrapper to the real security-scoped NSURL it
// carries (Task 13). The wrapper is a modern icdd type with NO public header, so
// the destination arrived under ICSecurityScopedWrappedURL as this opaque object
// and the Task-12 build logged "unexpected value class" and fell back to the
// plain path. Clean-room: rather than assume one accessor, probe the object's
// runtime interface -- the public-looking, zero-argument, object-returning
// selectors a URL wrapper would plausibly expose -- and, failing those, KVC.
// Each dynamic call is guarded so an unexpected shape can never throw into the
// callback, and if nothing yields an NSURL the wrapper's full method list is
// logged so a follow-up can pin the exact accessor. Returns an autoreleased
// NSURL (or nil); the caller retains what it keeps.
NSURL* UnwrapSecurityScopedURL(id wrapper) {
  if (wrapper == nil) return nil;

  // Zero-arg, object-returning accessors a scoped-URL wrapper plausibly exposes.
  static const char* const kSelectorNames[] = {
      "URL", "url", "fileURL", "securityScopedURL", "scopedURL",
      "nsURL", "wrappedURL"};
  for (const char* name : kSelectorNames) {
    SEL sel = sel_getUid(name);
    if (![wrapper respondsToSelector:sel]) continue;
    @try {
      // Call through a typed objc_msgSend cast so the object return uses the
      // right calling convention; the result is an autoreleased NSURL by naming
      // convention (no alloc/new/copy family), which the caller retains to keep.
      id result = ((id (*)(id, SEL))objc_msgSend)(wrapper, sel);
      if ([result isKindOfClass:[NSURL class]]) {
        os_log(Log(), "UnwrapSecurityScopedURL: unwrapped via -%{public}s", name);
        return (NSURL*)result;
      }
    } @catch (NSException* e) {
      os_log_error(Log(),
                   "UnwrapSecurityScopedURL: -%{public}s threw %{public}@", name,
                   e.reason);
    }
  }

  // KVC fallback for a backing property not exposed as a callable selector.
  for (NSString* key in @[ @"URL", @"url", @"fileURL" ]) {
    @try {
      id result = [wrapper valueForKey:key];
      if ([result isKindOfClass:[NSURL class]]) {
        os_log(Log(), "UnwrapSecurityScopedURL: unwrapped via KVC '%{public}@'",
               key);
        return (NSURL*)result;
      }
    } @catch (NSException* e) {
      // valueForKey: throws for an unknown key; try the next candidate.
    }
  }

  // Nothing worked: dump the class's methods so the accessor can be pinned.
  NSMutableArray<NSString*>* methods = [NSMutableArray array];
  unsigned int count = 0;
  Method* list = class_copyMethodList([wrapper class], &count);
  if (list) {
    for (unsigned int i = 0; i < count; ++i) {
      [methods addObject:NSStringFromSelector(method_getName(list[i]))];
    }
    free(list);
  }
  os_log_error(Log(),
               "UnwrapSecurityScopedURL: no accessor yielded an NSURL; "
               "class=%{public}@ methods=%{public}@",
               [wrapper class], methods);
  return nil;
}

// Resolves the host's security-scoped destination folder to a retained CFURLRef
// (caller owns the +1; ReleaseScopedURL frees it), or null if absent/unusable.
// The value under ICSecurityScopedWrappedURL is NOT declared in any public SDK
// header -- it is a modern icdd key observed only in our own live logs -- so its
// concrete type is handled defensively: an NSURL is used as-is; NSData is
// resolved as a security-scoped bookmark; an NSString is treated as a file path;
// and the observed NSSecurityScopedURLWrapper is unwrapped clean-room (Task 13).
CFURLRef ResolveSecurityScopedURL(CFDictionaryRef dict) {
  id value = LookupParamValue(dict, @"ICSecurityScopedWrappedURL");
  if (value == nil) return nullptr;

  NSURL* url = nil;
  if ([value isKindOfClass:[NSURL class]]) {
    url = (NSURL*)value;
  } else if ([value isKindOfClass:[NSData class]]) {
    NSError* err = nil;
    BOOL stale = NO;
    url = [NSURL URLByResolvingBookmarkData:(NSData*)value
                                   options:NSURLBookmarkResolutionWithSecurityScope
                             relativeToURL:nil
                       bookmarkDataIsStale:&stale
                                     error:&err];
    os_log(Log(),
           "ResolveSecurityScopedURL: bookmark resolve stale=%d err=%{public}@",
           stale, err);
  } else if ([value isKindOfClass:[NSString class]]) {
    url = [NSURL fileURLWithPath:(NSString*)value];
  } else {
    // Not a public type -- the observed NSSecurityScopedURLWrapper. Unwrap it to
    // the real scoped NSURL clean-room; if that fails, DetectTransferMode still
    // keeps the plain "document folder" path as the primary destination.
    url = UnwrapSecurityScopedURL(value);
    if (url == nil) {
      os_log_error(Log(),
                   "ResolveSecurityScopedURL: could not unwrap value class "
                   "%{public}@ (falling back to plain path)",
                   [value class]);
      return nullptr;
    }
  }
  if (url == nil) return nullptr;
  return static_cast<CFURLRef>(CFRetain((__bridge CFTypeRef)url));
}

// Detects the transfer mode from a SetParameters request and records it on the
// device context: a FINAL scan carries a security-scoped destination URL and/or
// a "document folder" path plus document name/format/extension keys -> file
// based transfer; an overview/preview scan carries none of these -> the
// in-memory image path. It stages the result on ctx for the next Start to
// consume. Because a scan runs synchronously inside Start (Task 16),
// SetParameters can never overlap a running scan, so releasing ctx's staged
// scoped URL here is safe.
void DetectTransferMode(DeviceContext* ctx, CFDictionaryRef dict) {
  if (ctx == nullptr) return;
  ReleaseScopedURL(ctx);  // Drop any prior request's staged scoped URL.
  ctx->fileTransfer = false;
  ctx->documentFolderPath.clear();

  const std::string docFolder = LookupStringParam(dict, @"document folder");
  const std::string docName = LookupStringParam(dict, @"document name");
  const std::string docExt = LookupStringParam(dict, @"document extension");
  const std::string docFormat = LookupStringParam(dict, @"document format");
  CFURLRef scoped = ResolveSecurityScopedURL(dict);

  if (scoped != nullptr || !docFolder.empty()) {
    ctx->fileTransfer = true;
    ctx->securityScopedURL = scoped;  // Owns the +1 (may be null).
    ctx->documentFolderPath = docFolder;
    ctx->transferPlan = brscan::ica::PlanTransfer(docFormat, docExt, docName);
    os_log(Log(),
           "SetParameters: FILE transfer -> folder='%{public}s' scopedURL=%d "
           "uti=%{public}s ext=%{public}s stem=%{public}s",
           docFolder.c_str(), scoped != nullptr,
           ctx->transferPlan.uti.c_str(), ctx->transferPlan.extension.c_str(),
           ctx->transferPlan.stem.c_str());
  } else {
    if (scoped != nullptr) CFRelease(scoped);
    os_log(Log(), "SetParameters: MEMORY/overview transfer (no destination)");
  }
}

ICAError SetParameters(const ScannerObjectInfo* deviceObjectInfo,
                       ICD_ScannerSetParametersPB* pb) {
  os_log(Log(), "callback: ICD_ScannerSetParameters");
  if (pb == nullptr) return kICADeviceInvalidParamErr;
  DeviceContext* ctx = ContextOf(deviceObjectInfo);
  if (pb->theDict) {
    // Full per-key dump (a whole-dict %@ truncates and hid the transfer keys).
    LogFullDict("SetParameters.dict", pb->theDict);

    // Translate the host's selection to a brscan::Params and stash it on the
    // device context for ICD_ScannerStart to scan with. The tracked unit (from an
    // earlier unit-switch SetParameters) is the last source fallback, so pass it
    // in; -1 means "no context" so the fallback is skipped.
    const int trackedUnit = ctx ? ctx->selectedFunctionalUnit : -1;
    brscan::ica::ScanRequest req = ReadScanRequest(pb->theDict, trackedUnit);
    brscan::ica::ScanLimits limits;  // default max_dpi = highest offer (600).
    brscan::Params params = brscan::ica::TranslateScanParams(req, limits);
    if (ctx) ctx->params = params;

    // Track the host's functional-unit selection so the next GetParameters
    // advertises it (0=flatbed, 3=feeder). Update only on an EXPLICIT unit or a
    // CAP_FEEDERENABLED signal -- a source resolved from the tracked value would
    // just rewrite the same value, and logging it every scan would be noise. On
    // the unit-switch round-trip the host sends ONLY selectedFunctionalUnitType
    // (no resolution/pixeltype); answering GetParameters with this tracked value
    // is what stops the feeder-selection loop.
    if (ctx &&
        (req.source_signal == brscan::ica::SourceSignal::kExplicitUnit ||
         req.source_signal == brscan::ica::SourceSignal::kFeederEnabled)) {
      ctx->selectedFunctionalUnit = req.functional_unit;
      os_log(Log(),
             "SetParameters: tracked selectedFunctionalUnitType=%d (%{public}s) "
             "via %{public}s",
             req.functional_unit,
             req.functional_unit == 3 ? "feeder" : "flatbed",
             SourceSignalName(req.source_signal));
    }

    // Detect file-based vs overview/memory transfer for this scan.
    DetectTransferMode(ctx, pb->theDict);

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
// SCAN EXECUTION THREAD (Task 16, authoritative -- reverses decision D):
//   The ENTIRE scan runs SYNCHRONOUSLY on icdd's callback thread, inside
//   ICD_ScannerStart. ICA notification delivery/reply is bound to the icdd
//   connection runloop on that callback thread, so notifications posted from any
//   other thread return noErr yet never reach the host. The earlier background
//   std::thread worker is exactly why the overview never rendered, the UI stuck
//   on "Scanner is warming up", and the scan never completed. Start now connects,
//   runs RunScan, hands every page back, and sends ScannerScanDone all inline,
//   returning only after ScanDone. Blocking Start for the whole scan is correct
//   and expected by icdd (all four reference modules do exactly this).
//
// HAND-BACK ORCHESTRATION (Task 18b -- LIVE bands; supersedes the Task-14
// whole-page hand-back. Interface facts only, clean-room, no source copied):
//   - WARM-UP: dropped in Task 16. The shipping Tahoe fork sends no warm-up
//     notifications; ours (posted from the worker thread) is what left the UI
//     stuck on "Scanner is warming up", so PostWarmUp and its calls are removed.
//   - The pixels stream back LIVE, one decoded band at a time, via
//     brscan::RunScan's 4-argument streaming overload (an on_band callback that
//     fires as rows decode off the wire). Each band is delivered INLINE in a
//     kICANotificationTypeScanProgressStatus packed with
//     ICDAddImageInfoToNotificationDictionary -- the IMAGE-info packer, NOT the
//     BAND-info packer: it populates the notification's kICANotificationImage*
//     keys (image data + width/height/bytesPerRow/startRow/numberOfRows,
//     documented in ICAApplication.h) that the host accumulates into the
//     overview/preview image (ICScannerFunctionalUnit's overviewImage). A band
//     carries the FULL page width/height with this band's dataStartRow /
//     dataNumberOfRows, so the preview fills top-to-bottom with a moving
//     progress bar DURING the scan. Task 11 used the BAND-info packer instead;
//     the host accepted the notification (replyCode ok) but never rendered the
//     overview, because its preview accumulator consumes the Image-info keys,
//     not the Band-info keys -- THIS was Defect A. Task 14 then sent the whole
//     page as one full-height Image-info chunk (dataStartRow=0), which rendered
//     only at the very end; Task 18b restores true progressive delivery.
//   - ICDSendNotificationAndWaitForReply keeps each band's bytes alive until
//     icdd copies them and surfaces a user cancel in replyCode; a cancel makes
//     the on_band callback return false, so RunScan stops reading promptly and
//     returns Status::kCancelled -- a true mid-scan cancel, not the old
//     page-boundary-only cancel.
//   - EVERY scanner notification references the framework-assigned DEVICE object
//     (ScannerObjectInfo::icaObject, marked "Apple" in ICD_ScannerCalls.h) under
//     kICANotificationICAObjectKey. Task 15 corrected this: prior builds used
//     ICD_ScannerStartPB::object (the scan trigger), and the host -- which keys
//     its scan session on the device object -- never completed the scan
//     ("Scanning document" hang) nor rendered the overview. The device object is
//     captured in OpenSession/Start from deviceObjectInfo->icaObject.
//     It does NOT use kICANotificationDeviceICAObjectKey here.
//   - MEMORY / overview path: the bands ARE the complete delivery. There is no
//     end-of-scan whole-image ScanProgressStatus and no per-page
//     ScannerPageDone; a single kICANotificationTypeScannerScanDone ends the job.
//   - FILE / final path: the same bands stream back to drive the progress bar,
//     AND RunScan still accumulates each whole page into `out`, so after RunScan
//     returns the finished page is encoded to the destination file and a
//     kICANotificationTypeScannerPageDone carrying the file path (under
//     kICANotificationScannerDocumentNameKey) is posted, then a single
//     ScannerScanDone (Task 12 file write, unchanged). Bands = progress; the
//     file write + PageDone = the actual delivery.
//
// LOGGING: band granularity is 16 rows, so a page produces hundreds of bands.
// PostBand logs the first band's image-info args once (logArgs); the loop logs a
// final per-page band count. Nothing is logged per band, so the unified log does
// not flood.

// userCanceledErr (MacErrors.h, -128) as it arrives in the UInt32 replyCode of a
// waited-for notification: the host sets it when the user cancels the scan.
constexpr UInt32 kUserCanceledReplyCode = static_cast<UInt32>(-128);

// ADF feeder-empty outcome, reported in the ScannerScanDone kICAErrorKey when an
// ADF scan produced no page (see RunScanSynchronous). Neither the module-side
// ICAError enum (ICADevices/ICAApplication.h: kICACommunicationErr = -9900 …
// kICASecureSessionRequired = -9923) nor the client-facing ICReturn enum
// (ImageCaptureCore/ImageCaptureConstants.h) defines a "no documents in the
// feeder" / paper-empty code. The closest is ICReturnScannerFailedToCompleteScan
// (ImageCaptureConstants.h, -9931) -- the scan-did-not-complete code Image
// Capture surfaces on the scanner path -- which is strictly more specific than
// the generic kICADeviceInternalErr (-9912) the module reported for every non-OK
// status. The kICAErrorKey value flows through to the client by number (the
// -9912 baseline confirms this), so it is used as a value only, not imported
// (matching kUserCanceledReplyCode above; ICADevices does not declare it).
constexpr ICAError kAdfFeederEmptyError = -9931;  // ICReturnScannerFailedToCompleteScan

// Outcome of handing one page back: delivered, host cancelled at this page
// boundary (progress replyCode == userCanceledErr), or a delivery/encode error.
enum class PageResult { kOk, kCanceled, kError };

// Sets the type + the device/scanner ICAObject (under kICANotificationICAObjectKey,
// the key every scanner notification carries) and pushes the dictionary to the
// host. `waitForReply` uses ICDSendNotificationAndWaitForReply (so the image
// buffer stays alive until icdd has copied it, and a user cancel surfaces in
// replyCode) for the data-bearing ScanProgressStatus; the page/scan-done signals
// are fire-and-forget. When `outReplyCode` is non-null it receives the host's
// reply code (only meaningful for a waited-for send). Returns the framework's
// ICAError.
ICAError SendScannerNotification(CFMutableDictionaryRef dict,
                                 ICAObject icaObject, CFStringRef type,
                                 bool waitForReply,
                                 UInt32* outReplyCode = nullptr) {
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
  const ICAError err = waitForReply ? ICDSendNotificationAndWaitForReply(&pb)
                                    : ICDSendNotification(&pb);
  if (outReplyCode) *outReplyCode = pb.replyCode;
  return err;
}

// Reports the document feeder as empty to the host: posts a
// kICANotificationTypeDeviceStatusInfo carrying kICANotificationSubTypeKey =
// kICANotificationSubTypeDocumentNotLoaded, referenced to the DEVICE object like
// every scanner notification (Task 15). This is the module-side path behind the
// client's readonly ICScannerFunctionalUnitDocumentFeeder.documentLoaded, which
// ImageCaptureCore/ICScannerFunctionalUnits.h documents as changing "if the
// scanner module has the capability to detect this state". The subtype-key
// mechanism mirrors the documented WarmUp* status notifications
// (kICANotificationSubTypeWarmUpStarted/Done ride the same key + type); all four
// symbols resolve from ICADevices.
//
// CLEAN-ROOM / UNVERIFIED: that the host maps DocumentNotLoaded onto
// documentLoaded = NO (and surfaces a feeder-empty message) is inferred from the
// public header names, not confirmed against a live icdd trace -- see the
// device-in-the-loop re-test in docs/ICA-PROTOCOL.md.
void NotifyDocumentFeederEmpty(ICAObject deviceObject) {
  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (dict == nullptr) return;
  CFDictionarySetValue(dict, kICANotificationSubTypeKey,
                       kICANotificationSubTypeDocumentNotLoaded);
  const ICAError err = SendScannerNotification(
      dict, deviceObject, kICANotificationTypeDeviceStatusInfo,
      /*waitForReply=*/false);
  CFRelease(dict);
  os_log(Log(),
         "SyncScan: posted DeviceStatusInfo/DocumentNotLoaded (feeder empty) "
         "err=%d",
         err);
}

// Hands ONE live band back as an IMAGE-info chunk in a ScanProgressStatus
// (Plan 2 Task 18b). `band.data` points at `band.size` host-ready bytes for the
// band's `num_rows` rows -- interleaved 24-bit RGB for kRgb, raw 8-bit gray for
// kGray, packed 1-bpp for kBitonal -- already decoded by RunScan's streaming
// overload (the module does NOT decode a band). The image-info args come from
// DescribeBand, which enforces the per-band stride guard (band.size must equal
// DescribeBuffer stride * num_rows; a mismatch renders nothing, so it is
// rejected). `icaObject` is the DEVICE object (Task 15). The send waits for the
// reply so the band buffer outlives icdd's copy and a user cancel surfaces in
// replyCode. `logArgs` gates the verbose per-band arg line: at 16-row band
// granularity a page has hundreds of bands, so the caller logs only the first
// band + a final count and passes false otherwise. Returns kOk if the band was
// accepted, kCanceled if the host replied userCanceledErr (host cancel), or
// kError on bad geometry / stride mismatch / a failed send.
PageResult PostBand(ICAObject icaObject, const brscan::ScanBand& band,
                    bool logArgs) {
  std::optional<brscan::ica::BandImageInfo> info = brscan::ica::DescribeBand(
      band.format, band.full_width, band.full_height, band.start_row,
      band.num_rows, band.size);
  if (!info) {
    os_log_error(Log(),
                 "PostBand[p%d]: bad band geometry/stride full=%dx%d "
                 "start=%d rows=%d size=%zu (dropped)",
                 band.page_index, band.full_width, band.full_height,
                 band.start_row, band.num_rows, band.size);
    return PageResult::kError;
  }
  if (band.data == nullptr) {
    os_log_error(Log(), "PostBand[p%d]: null band data", band.page_index);
    return PageResult::kError;
  }

  // Log the first band's exact image-info args once per scan so the unified log
  // is not flooded by the hundreds of 16-row bands a page produces.
  if (logArgs) {
    os_log(Log(),
           "PostBand[p%d]: first-band image-info args width=%lld height=%lld "
           "bytesPerRow=%lld dataStartRow=%lld dataNumberOfRows=%lld "
           "dataSize=%lld format=%{public}s",
           band.page_index, (long long)info->width, (long long)info->height,
           (long long)info->bytes_per_row, (long long)info->data_start_row,
           (long long)info->data_number_of_rows, (long long)info->data_size,
           PixelFormatName(band.format));
  }

  // Populate the kICANotificationImage* keys the host's overview/preview
  // accumulator consumes -- the full page width/height with this band's row
  // offset/count -- so the preview fills top-to-bottom as bands arrive.
  CFMutableDictionaryRef imageDict = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (imageDict == nullptr) return PageResult::kError;

  ICAError addErr = ICDAddImageInfoToNotificationDictionary(
      imageDict, static_cast<UInt32>(info->width),
      static_cast<UInt32>(info->height),
      static_cast<UInt32>(info->bytes_per_row),
      static_cast<UInt32>(info->data_start_row),
      static_cast<UInt32>(info->data_number_of_rows),
      static_cast<UInt32>(info->data_size), const_cast<uint8_t*>(band.data));

  UInt32 replyCode = 0;
  ICAError imageErr = SendScannerNotification(
      imageDict, icaObject, kICANotificationTypeScanProgressStatus,
      /*waitForReply=*/true, &replyCode);
  CFRelease(imageDict);

  if (replyCode == kUserCanceledReplyCode) {
    os_log(Log(),
           "PostBand[p%d]: host cancel (progress replyCode=userCanceledErr) at "
           "startRow=%d",
           band.page_index, band.start_row);
    return PageResult::kCanceled;
  }
  if (addErr != noErr || imageErr != noErr) {
    os_log_error(Log(),
                 "PostBand[p%d]: send failed addImageInfo=%d sendProgress=%d",
                 band.page_index, addErr, imageErr);
    return PageResult::kError;
  }
  return PageResult::kOk;
}

// ---------------------------------------------------------------------------
// File-based transfer (Task 12).
//
// When the host asks for a saved file (see the transfer-mode note above and
// PLAN-2-DESIGN.md), the module encodes each decoded page with native macOS
// ImageIO (CGImageDestination) and writes it into the host's destination
// folder, then posts the page/scan-done notifications the host recognises for a
// saved file. This is entirely self-contained -- it does NOT use
// daemon/output_writer (different scope). CGImage construction facts are the
// public CoreGraphics/ImageIO contract; the notification sequence is confirmed
// from Apple's ICADevices sample VirtualScanner (facts only, no source copied):
// its file transfer writes the page with CGImageDestinationCreateWithURL / ...
// AddImage / ...Finalize, posts a kICANotificationTypeScannerPageDone carrying
// the file path under kICANotificationScannerDocumentNameKey, and ends the job
// with a single kICANotificationTypeScannerScanDone.

// Builds a CGImage for one decoded page. `bytes`/`byteCount` are the host-ready
// pixels: interleaved 24-bit RGB (post-DecodeJpeg) for kRgb, raw 8-bit gray for
// kGray, packed 1-bpp MSB-first (1 = black) for kBitonal. The image borrows
// `bytes` through a no-copy data provider, so `bytes` MUST outlive the returned
// image (the caller releases it right after the encode, while `bytes` is still
// on the stack). Caller owns the result (CGImageRelease). Returns null on bad
// geometry, short buffer, or a CoreGraphics failure.
CGImageRef CreatePageImage(brscan::PixelFormat format, const uint8_t* bytes,
                           size_t byteCount, int width, int height) {
  std::optional<brscan::ica::BufferDescriptor> d =
      brscan::ica::DescribeBuffer(format, width, height);
  if (!d) return nullptr;
  if (bytes == nullptr ||
      byteCount < static_cast<size_t>(d->expected_byte_count)) {
    return nullptr;
  }

  CGColorSpaceRef cs = (format == brscan::PixelFormat::kRgb)
                           ? CGColorSpaceCreateDeviceRGB()
                           : CGColorSpaceCreateDeviceGray();
  if (cs == nullptr) return nullptr;

  CGDataProviderRef provider =
      CGDataProviderCreateWithData(nullptr, bytes, byteCount, nullptr);
  if (provider == nullptr) {
    CGColorSpaceRelease(cs);
    return nullptr;
  }

  const size_t bitsPerComponent =
      (format == brscan::PixelFormat::kBitonal) ? 1 : 8;

  // Bitonal: libbrscan packs 1 = black, but a 1-bit gray CGImage maps bit 1 ->
  // white. A {1,0} decode array flips the polarity so ink stays black.
  const CGFloat bitonalDecode[2] = {1.0, 0.0};
  const CGFloat* decode =
      (format == brscan::PixelFormat::kBitonal) ? bitonalDecode : nullptr;

  CGImageRef img = CGImageCreate(
      static_cast<size_t>(width), static_cast<size_t>(height), bitsPerComponent,
      static_cast<size_t>(d->bits_per_pixel),
      static_cast<size_t>(d->bytes_per_row), cs,
      kCGImageAlphaNone | kCGBitmapByteOrderDefault, provider, decode,
      /*shouldInterpolate=*/false, kCGRenderingIntentDefault);

  CGDataProviderRelease(provider);
  CGColorSpaceRelease(cs);
  return img;
}

// Encodes `image` to `fileURL` as the ImageIO type `uti`. Returns true on a
// finalized write.
bool WriteImageToURL(CGImageRef image, NSURL* fileURL, CFStringRef uti) {
  if (image == nullptr || fileURL == nil || uti == nullptr) return false;
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)fileURL, uti, 1, nullptr);
  if (dst == nullptr) return false;
  CGImageDestinationAddImage(dst, image, nullptr);
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  return ok;
}

// Writes one decoded page to the file-based destination and signals the page
// done. Resolves the destination folder (security-scoped URL preferred, plain
// "document folder" path as fallback), start/stop-accessing the scoped resource
// around the write, encodes via ImageIO, then posts a ScannerPageDone carrying
// the written file path under kICANotificationScannerDocumentNameKey. The
// scoped URL is borrowed (owned by Start, which releases it after the scan);
// this only starts/stops access, it does not retain or release. Returns kOk if
// the file was written, kError otherwise. (The file path posts page-done
// fire-and-forget, so it never observes a host cancel -- cancel is detected on
// the in-memory overview path.)
PageResult PostFilePage(CFURLRef securityScopedURL,
                        const std::string& documentFolderPath,
                        const brscan::ica::TransferPlan& transferPlan,
                        ICAObject icaObject, brscan::PixelFormat format,
                        const uint8_t* bytes, size_t byteCount, int width,
                        int height, int pageIndex) {
  bool wrote = false;
  std::string writtenPath;
  @autoreleasepool {
    NSURL* folderURL = nil;
    BOOL accessing = NO;
    const char* pathKind = "none";
    if (securityScopedURL != nullptr) {
      // Scoped destination: start accessing the security-scoped resource for the
      // duration of the write, then stop below. The URL is owned by Start.
      folderURL = (__bridge NSURL*)securityScopedURL;
      accessing = [folderURL startAccessingSecurityScopedResource];
      pathKind = "scoped";
    } else if (!documentFolderPath.empty()) {
      NSString* p =
          [[NSString stringWithUTF8String:documentFolderPath.c_str()]
              stringByExpandingTildeInPath];
      folderURL = [NSURL fileURLWithPath:p isDirectory:YES];
      pathKind = "plain";
    }
    if (folderURL == nil) {
      os_log_error(Log(), "PostFilePage[%d]: no destination folder", pageIndex);
      return PageResult::kError;
    }
    os_log(Log(),
           "PostFilePage[%d]: destination path=%{public}s scopedAccess=%d",
           pageIndex, pathKind, accessing);

    const std::string filename =
        brscan::ica::TransferFilenameForPage(transferPlan, pageIndex);
    NSURL* fileURL = [folderURL
        URLByAppendingPathComponent:[NSString
                                        stringWithUTF8String:filename.c_str()]];

    CGImageRef image = CreatePageImage(format, bytes, byteCount, width, height);
    if (image != nullptr) {
      NSString* utiStr =
          [NSString stringWithUTF8String:transferPlan.uti.c_str()];
      os_log(Log(),
             "file transfer: writing %{public}@ format=%{public}s %dx%d",
             fileURL.path, transferPlan.uti.c_str(), width, height);
      wrote = WriteImageToURL(image, fileURL, (__bridge CFStringRef)utiStr);
      CGImageRelease(image);
    } else {
      os_log_error(Log(), "PostFilePage[%d]: CGImage build failed %dx%d",
                   pageIndex, width, height);
    }

    if (accessing) [folderURL stopAccessingSecurityScopedResource];

    if (wrote) {
      NSNumber* size = nil;
      [fileURL getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
      const char* pathC = fileURL.path.UTF8String;
      writtenPath = pathC ? pathC : "";
      os_log(Log(), "file transfer: wrote %lld bytes to %{public}@",
             (long long)size.longLongValue, fileURL.path);
    }

    // Page-done notification: reference the DEVICE object like every scanner
    // notification (Task 15), and carry the EXACT written file path under the
    // document-name key -- the same absolute path the file was written to
    // (`<document folder>/<document name>.<document extension>`), so the host
    // picks up the saved file. Log the exact value sent.
    os_log(Log(),
           "PostFilePage[%d]: documentName key path=%{public}s (wrote=%d)",
           pageIndex, writtenPath.c_str(), wrote);
    CFMutableDictionaryRef pageDict = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    ICAError pageErr = kICADeviceInvalidParamErr;
    if (pageDict) {
      if (wrote && !writtenPath.empty()) {
        CFStringRef cfPath = CFStringCreateWithCString(
            nullptr, writtenPath.c_str(), kCFStringEncodingUTF8);
        if (cfPath) {
          CFDictionarySetValue(pageDict, kICANotificationScannerDocumentNameKey,
                               cfPath);
          CFRelease(cfPath);
        }
      }
      pageErr = SendScannerNotification(pageDict, icaObject,
                                        kICANotificationTypeScannerPageDone,
                                        /*waitForReply=*/false);
      CFRelease(pageDict);
    }
    os_log(Log(), "PostFilePage[%d]: wrote=%d sendPageDone=%d", pageIndex, wrote,
           pageErr);
  }
  return wrote ? PageResult::kOk : PageResult::kError;
}

// Runs the ENTIRE scan SYNCHRONOUSLY on icdd's callback thread (Task 16),
// returning only after ScannerScanDone. ICA notification delivery/reply is bound
// to the icdd connection runloop on the callback thread, so the scan MUST run
// here -- notifications posted from any other thread return noErr yet never reach
// the host (the earlier background-thread bug). `transport`, the destination URL,
// and every page buffer are locals owned by the caller (Start), so there is no
// worker, no join, and no cross-thread state. `securityScopedURL` is borrowed
// (Start owns and releases it). Returns the ICAError reported to the host in the
// final ScannerScanDone (noErr on success or a clean host cancel).
ICAError RunScanSynchronous(const DeviceContext& ctx, ICAObject deviceObject,
                            const brscan::Params& params, bool fileTransfer,
                            const brscan::ica::TransferPlan& transferPlan,
                            const std::string& documentFolderPath,
                            CFURLRef securityScopedURL) {
  os_log(Log(),
         "SyncScan: begin ip='%{public}s' port=%d mode=%{public}s dpi=%d "
         "source=%{public}s duplex=%d fileTransfer=%d",
         ctx.ipAddress.c_str(), ctx.port, ModeName(params.mode), params.x_dpi,
         SourceName(params.source), params.duplex, fileTransfer);

  brscan::TcpTransport transport(ctx.ipAddress,
                                 static_cast<uint16_t>(ctx.port));

  ICAError finalErr = noErr;
  bool canceled = false;

  // Bounded connect retry (Task 11): the Brother device allows a single scan
  // connection, so a lingering socket from a prior session can lose the first
  // race (observed live: connect -> 1, then success on retry). Try up to
  // kConnectAttempts times, ~kConnectBackoff apart. std::this_thread::sleep_for
  // is used only for this backoff -- there is no scan worker thread (Task 16).
  constexpr int kConnectAttempts = 3;
  constexpr auto kConnectBackoff = std::chrono::milliseconds(500);
  brscan::Status connectStatus = brscan::Status::kIoError;
  for (int attempt = 1; attempt <= kConnectAttempts; ++attempt) {
    connectStatus = transport.Connect();
    os_log(Log(), "SyncScan: transport connect attempt %d/%d -> %d", attempt,
           kConnectAttempts, (int)connectStatus);
    if (connectStatus == brscan::Status::kOk) break;
    if (attempt < kConnectAttempts) {
      std::this_thread::sleep_for(kConnectBackoff);
    }
  }

  if (connectStatus != brscan::Status::kOk) {
    finalErr = kICADeviceInternalErr;
  } else {
    std::vector<brscan::ScanResult> pages;

    // Live band streaming (Task 18b): every decoded band is pushed to the host
    // as an IMAGE-info ScanProgressStatus for BOTH transfer modes -- that is
    // what fills the overview top-to-bottom and drives the progress bar DURING
    // the scan. RunScan's streaming overload also accumulates each whole page
    // into `pages`, so the file path can still encode+write the finished page
    // after the scan. The callback runs inline on this (icdd callback) thread,
    // so its notifications are delivered on the connection runloop; returning
    // false makes RunScan stop reading promptly and return kCancelled (a true
    // mid-scan cancel). To avoid flooding the unified log at 16-row band
    // granularity, PostBand logs only the first band's args (logArgs) and a
    // per-scan band count is logged once RunScan returns.
    bool loggedFirstBand = false;
    long bandCount = 0;
    const brscan::BandCallback onBand =
        [&](const brscan::ScanBand& band) -> bool {
      const bool logArgs = !loggedFirstBand;
      loggedFirstBand = true;
      ++bandCount;
      const PageResult r = PostBand(deviceObject, band, logArgs);
      if (r == PageResult::kCanceled) {
        canceled = true;
        return false;  // Host cancel -> RunScan returns kCancelled.
      }
      return true;  // kOk, or a single dropped band -- keep scanning.
    };

    const brscan::Status scanStatus =
        brscan::RunScan(transport, params, &pages, onBand);
    os_log(Log(),
           "SyncScan: RunScan -> status=%d pages=%zu bands=%ld canceled=%d",
           (int)scanStatus, pages.size(), bandCount, canceled);

    // Classify the finished scan (pure; unit-tested in scan_outcome_test).
    const brscan::ica::ScanOutcome outcome = brscan::ica::ClassifyScanOutcome(
        params.source, /*produced_pages=*/!pages.empty(), scanStatus);

    if (outcome == brscan::ica::ScanOutcome::kCanceled) {
      // Clean host cancel: the bands already delivered stay on the host; write
      // no file and end with a clean ScannerScanDone (Start releases the scoped
      // URL, and transport is disconnected below -- nothing leaks).
      canceled = true;
    } else if (outcome == brscan::ica::ScanOutcome::kAdfFeederEmpty) {
      // ADF with an empty feeder: no page ever came off the feeder (kNoPaper),
      // or the wait elapsed with nothing fed (kTimeout -- the ~24 s hang until
      // fast empty-ADF detection lands in libbrscan from a wire capture; the
      // timeout is deliberately NOT shortened here). Report a clean feeder-empty
      // outcome instead of the generic kICADeviceInternalErr: tell the host the
      // feeder has no document loaded (so the client's readonly documentLoaded
      // reflects it) and end the scan with the feeder-empty error. Flatbed, and
      // any scan that produced pages, are unaffected.
      NotifyDocumentFeederEmpty(deviceObject);
      finalErr = kAdfFeederEmptyError;
      os_log(Log(),
             "SyncScan: ADF empty (status=%d, pages=0) -> feeder-empty err=%d "
             "(ICReturnScannerFailedToCompleteScan)",
             (int)scanStatus, finalErr);
    } else if (outcome == brscan::ica::ScanOutcome::kFailed) {
      finalErr = kICADeviceInternalErr;
    } else if (fileTransfer) {
      // FILE path only: the bands drove the progress bar; now encode each whole
      // page to the destination file and post ScannerPageDone(path). The MEMORY
      // path needs no post-processing -- its bands were the complete delivery,
      // so it sends only the final ScannerScanDone below.
      int idx = 0;
      for (const brscan::ScanResult& page : pages) {
        os_log(Log(), "SyncScan: file page %d %dx%d format=%{public}s payload=%zu",
               idx, page.width, page.height, PixelFormatName(page.format),
               page.data.size());

        // Normalise the page to host-ready bytes: color pages arrive in `pages`
        // as baseline JPEG (bands were decoded separately) and are decoded to
        // interleaved RGB here for the file encode; gray/bitonal pass through.
        // `img` must outlive PostFilePage (it backs the RGB bytes).
        brscan::Image img;
        const uint8_t* bytes = nullptr;
        size_t byteCount = 0;
        int outWidth = page.width;
        int outHeight = page.height;
        brscan::PixelFormat outFormat = page.format;
        bool ready = true;
        if (page.format == brscan::PixelFormat::kRgb) {
          const brscan::Status dec =
              brscan::DecodeJpeg(page.data.data(), page.data.size(), &img);
          if (dec != brscan::Status::kOk) {
            os_log_error(Log(), "SyncScan: page %d JPEG decode failed (%d)", idx,
                         (int)dec);
            finalErr = kICADeviceInternalErr;
            ready = false;
          } else {
            bytes = img.pixels.data();
            byteCount = img.pixels.size();
            outWidth = img.width;
            outHeight = img.height;
          }
        } else {
          bytes = page.data.data();
          byteCount = page.data.size();
        }

        if (ready) {
          PostFilePage(securityScopedURL, documentFolderPath, transferPlan,
                       deviceObject, outFormat, bytes, byteCount, outWidth,
                       outHeight, idx);
        }
        ++idx;
      }
    }
  }

  transport.Disconnect();

  // Completion: always tell the host the scan is done, carrying an error code
  // when one occurred (but not for a clean host cancel) so the scan UI does not
  // hang waiting for more pages.
  const ICAError doneErr = canceled ? noErr : finalErr;
  CFMutableDictionaryRef done = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (done) {
    if (doneErr != noErr) {
      CFNumberRef errNum =
          CFNumberCreate(nullptr, kCFNumberSInt32Type, &doneErr);
      if (errNum) {
        CFDictionarySetValue(done, kICAErrorKey, errNum);
        CFRelease(errNum);
      }
    }
    ICAError sendErr = SendScannerNotification(
        done, deviceObject, kICANotificationTypeScannerScanDone,
        /*waitForReply=*/false);
    CFRelease(done);
    os_log(Log(),
           "SyncScan: ScannerScanDone sent err=%d (finalErr=%d canceled=%d)",
           sendErr, finalErr, canceled);
  }
  os_log(Log(), "SyncScan: end (ran on callback thread)");
  return doneErr;
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

  // Notification object (Task 15 prime fix). Prior builds handed every
  // ScanProgressStatus / ScannerPageDone / ScannerScanDone the trigger's
  // ICD_ScannerStartPB::object; the host keys its scan session on the DEVICE
  // object (ScannerObjectInfo::icaObject, "Apple" in ICD_ScannerCalls.h), so
  // notifications against the wrong object never completed the scan or rendered
  // the overview. Prefer the captured device object; fall back to pb->object only
  // if the framework has not populated it.
  if (deviceObjectInfo && deviceObjectInfo->icaObject != 0) {
    ctx->deviceObject = deviceObjectInfo->icaObject;
  }
  const ICAObject notifyObject =
      ctx->deviceObject != 0 ? ctx->deviceObject : pb->object;
  os_log(Log(),
         "Start: notification object -> deviceObjectInfo->icaObject=0x%08x "
         "pb->object=0x%08x using=0x%08x",
         deviceObjectInfo ? deviceObjectInfo->icaObject : 0, pb->object,
         notifyObject);

  // Take the transfer settings + params as locals for this scan. SetParameters
  // and Start alternate on the one callback thread (Task 16), so no snapshot
  // object or lock is needed; ownership of the resolved security-scoped URL moves
  // out of ctx into this local so its access-token lifetime is exactly this scan
  // (released after RunScanSynchronous), leaving ctx with nothing to double-free.
  const brscan::Params params = ctx->params;
  const bool fileTransfer = ctx->fileTransfer;
  const brscan::ica::TransferPlan transferPlan = ctx->transferPlan;
  const std::string documentFolderPath = ctx->documentFolderPath;
  CFURLRef securityScopedURL = ctx->securityScopedURL;  // Take the +1.
  ctx->securityScopedURL = nullptr;

  os_log(Log(),
         "Start: running SYNCHRONOUS scan mode=%{public}s dpi=%d "
         "source=%{public}s duplex=%d area=(%d,%d,%d,%d) fileTransfer=%d",
         ModeName(params.mode), params.x_dpi, SourceName(params.source),
         params.duplex, params.area.x0, params.area.y0, params.area.x1,
         params.area.y1, fileTransfer);

  // Run the whole scan inline on icdd's callback thread; every notification
  // (progress/page-done/scan-done) is posted from here and returns only after
  // ScannerScanDone.
  const ICAError finalErr =
      RunScanSynchronous(*ctx, notifyObject, params, fileTransfer, transferPlan,
                         documentFolderPath, securityScopedURL);

  if (securityScopedURL) CFRelease(securityScopedURL);  // Release the +1.

  // Completion was already delivered via ScannerScanDone; report success from
  // Start itself so a synchronous failure is not double-counted by the host.
  os_log(Log(), "Start: scan complete (finalErr=%d), returning", finalErr);
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
