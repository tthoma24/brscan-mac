// Plan 2 Task 1 — ICA device-module load spike.
//
// This is NOT the scan path. It is the smallest bundle that the Image Capture
// discovery daemon (`icdd`) could load and match to a device, built to answer
// one question: does a third-party ICA scanner module still load on this macOS,
// and under what signing? See docs/RUNBOOK-plan-2-loadspike.md for the finding
// and the reproduction steps.
//
// Shape (confirmed against the macOS SDK's ICADevices.framework headers):
//   - The framework exports `int ICD_ScannerMain(int, const char**)` and the
//     global callback table `gICDScannerCallbackFunctions`
//     (ICADevices/ICD_ScannerCalls.h). A module fills in the entry points it
//     implements, then hands control to ICD_ScannerMain, which runs the
//     module's Mach service loop and calls back for each host request.
//   - The bundle is a background-only .app in /Library/Image Capture/Devices/;
//     icdd reads Contents/Resources/DeviceMatchingInfo.plist to learn which
//     Bonjour service type (_scanner._tcp) and devices this module matches.
//
// Every callback here is a stub that returns noErr and does no real work: the
// spike only needs the module to load, register, and let the synthetic device
// appear. The real callbacks (session, parameters, scan, band delivery) are
// Plan 2 Tasks 5-7 and are deliberately absent.
//
// Clean-room: written only against the public SDK headers; no Brother or Apple
// source consulted. The device identity is the synthetic BRW00AABBCCDDEE.

#import <Foundation/Foundation.h>
#import <ICADevices/ICADevices.h>
#import <os/log.h>

namespace {

// Tracing for the load spike. Item 2 of Plan 2 Task 1b: prove whether icdd
// loads our executable and calls our entry points at all, versus only reading
// the plists. Filter a live run with:
//
//   log stream --predicate 'subsystem == "me.tthoma24.brscan.ica"' --info --debug
//
// If NONE of these lines appear after a rescan, icdd never launched our
// executable (a signing/load gate, not a plist bug). If "ICD_ScannerMain
// entered" appears but no device shows, the module runs but the match/DeviceInfo
// side is still wrong.
os_log_t SpikeLog() {
  static os_log_t log =
      os_log_create("me.tthoma24.brscan.ica", "loadspike");
  return log;
}

// The scanner-specific entry points, each a no-op stub with the exact signature
// from ICD_ScannerCalls.h. Returning noErr (0) says "handled, no error". Each
// logs so the icdd log shows exactly which callbacks the host drives.

ICAError SpikeOpenTCPIPDevice(CFDictionaryRef /*params*/,
                              ScannerObjectInfo* /*objectInfo*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerOpenTCPIPDevice");
  return noErr;
}

ICAError SpikeCloseDevice(ScannerObjectInfo* /*objectInfo*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerCloseDevice");
  return noErr;
}

ICAError SpikePeriodicTask(ScannerObjectInfo* /*objectInfo*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerPeriodicTask");
  return noErr;
}

ICAError SpikeGetObjectInfo(const ScannerObjectInfo* /*parentInfo*/,
                            UInt32 /*index*/,
                            ScannerObjectInfo* /*newInfo*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerGetObjectInfo");
  return noErr;
}

ICAError SpikeCleanup(ScannerObjectInfo* /*objectInfo*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerCleanup");
  return noErr;
}

ICAError SpikeGetPropertyData(const ScannerObjectInfo* /*objectInfo*/,
                              void* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerGetPropertyData");
  return noErr;
}

ICAError SpikeSetPropertyData(const ScannerObjectInfo* /*objectInfo*/,
                              const void* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerSetPropertyData");
  return noErr;
}

ICAError SpikeOpenSession(const ScannerObjectInfo* /*deviceObjectInfo*/,
                          ICD_ScannerOpenSessionPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerOpenSession");
  return noErr;
}

ICAError SpikeCloseSession(const ScannerObjectInfo* /*deviceObjectInfo*/,
                           ICD_ScannerCloseSessionPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerCloseSession");
  return noErr;
}

ICAError SpikeInitialize(const ScannerObjectInfo* /*deviceObjectInfo*/,
                         ICD_ScannerInitializePB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerInitialize");
  return noErr;
}

ICAError SpikeGetParameters(const ScannerObjectInfo* /*deviceObjectInfo*/,
                            ICD_ScannerGetParametersPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerGetParameters");
  return noErr;
}

ICAError SpikeSetParameters(const ScannerObjectInfo* /*deviceObjectInfo*/,
                            ICD_ScannerSetParametersPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerSetParameters");
  return noErr;
}

ICAError SpikeStatus(const ScannerObjectInfo* /*deviceObjectInfo*/,
                     ICD_ScannerStatusPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerStatus");
  return noErr;
}

ICAError SpikeStart(const ScannerObjectInfo* /*deviceObjectInfo*/,
                    ICD_ScannerStartPB* /*pb*/) {
  os_log(SpikeLog(), "callback: ICD_ScannerStart");
  return noErr;
}

// Register the stubs the host is most likely to call while merely enumerating
// and opening the device. Members left NULL are entry points the spike does not
// implement; the framework must tolerate that for a load spike to be meaningful.
void RegisterCallbacks() {
  gICDScannerCallbackFunctions.f_ICD_ScannerOpenTCPIPDevice =
      SpikeOpenTCPIPDevice;
  gICDScannerCallbackFunctions.f_ICD_ScannerCloseDevice = SpikeCloseDevice;
  gICDScannerCallbackFunctions.f_ICD_ScannerPeriodicTask = SpikePeriodicTask;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetObjectInfo = SpikeGetObjectInfo;
  gICDScannerCallbackFunctions.f_ICD_ScannerCleanup = SpikeCleanup;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetPropertyData =
      SpikeGetPropertyData;
  gICDScannerCallbackFunctions.f_ICD_ScannerSetPropertyData =
      SpikeSetPropertyData;
  gICDScannerCallbackFunctions.f_ICD_ScannerOpenSession = SpikeOpenSession;
  gICDScannerCallbackFunctions.f_ICD_ScannerCloseSession = SpikeCloseSession;
  gICDScannerCallbackFunctions.f_ICD_ScannerInitialize = SpikeInitialize;
  gICDScannerCallbackFunctions.f_ICD_ScannerGetParameters = SpikeGetParameters;
  gICDScannerCallbackFunctions.f_ICD_ScannerSetParameters = SpikeSetParameters;
  gICDScannerCallbackFunctions.f_ICD_ScannerStatus = SpikeStatus;
  gICDScannerCallbackFunctions.f_ICD_ScannerStart = SpikeStart;
}

}  // namespace

int main(int argc, const char* argv[]) {
  // First line the icdd log should show if icdd launched our executable at all.
  os_log(SpikeLog(), "main: BrscanICALoadSpike executable launched (argc=%d)",
         argc);
  RegisterCallbacks();
  os_log(SpikeLog(), "main: callbacks registered, entering ICD_ScannerMain");
  // ICD_ScannerMain runs the module's service loop and does not return in
  // normal operation; the host tears the process down.
  int rc = ICD_ScannerMain(argc, argv);
  os_log(SpikeLog(), "main: ICD_ScannerMain returned %d (unexpected)", rc);
  return rc;
}
