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
#include <cstring>
#include <new>
#include <string>

#include "scan_parameters.h"

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

ICAError CloseDevice(ScannerObjectInfo* objectInfo) {
  os_log(Log(), "callback: ICD_ScannerCloseDevice");
  DeviceContext* ctx = ContextOf(objectInfo);
  if (ctx) {
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
  if (ctx) ctx->sessionOpen = false;
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
    brscan::ica::BuildScannerParameters(pb->theDict);
    os_log(Log(), "GetParameters: described %ld parameter keys",
           CFDictionaryGetCount(pb->theDict));
  } else {
    os_log_error(Log(), "GetParameters: theDict is null");
  }
  pb->header.err = noErr;
  return noErr;
}

ICAError SetParameters(const ScannerObjectInfo* /*deviceObjectInfo*/,
                       ICD_ScannerSetParametersPB* pb) {
  os_log(Log(), "callback: ICD_ScannerSetParameters");
  if (pb && pb->theDict) {
    os_log(Log(), "SetParameters: host set %ld keys: %{public}@",
           CFDictionaryGetCount(pb->theDict), (__bridge NSDictionary*)pb->theDict);
  }
  // Accept the selection; the values are translated to brscan::Params when the
  // scan-execution task wires ICD_ScannerStart to RunScan.
  if (pb) pb->header.err = noErr;
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

ICAError Start(const ScannerObjectInfo* /*deviceObjectInfo*/,
               ICD_ScannerStartPB* pb) {
  os_log(Log(),
         "callback: ICD_ScannerStart -> not implemented this task "
         "(returning kICADeviceUnsupportedErr; RunScan wiring is PLAN-2 task 7)");
  if (pb) pb->header.err = kICADeviceUnsupportedErr;
  return kICADeviceUnsupportedErr;
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
