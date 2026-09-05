import SwiftUI

/// The config window's content: a five-tab `TabView` keyed by `Tabs`, plus
/// (task 1e.9) a first-run banner and a Save bar, all driven by a single
/// `ConfigStore`. Task 1e.10 added a daemon-status line above the tabs and
/// a "Save & Apply" action alongside plain Save, both backed by a
/// `DaemonViewModel`.
///
/// The **General** tab (task 1e.6) is wired to a real `GeneralViewModel`;
/// **File** / **Image** / **Email** (task 1e.7) are each wired to their own
/// `RouteViewModel` via the shared `RouteEditorView`. **OCR** (task 1e.8) is
/// wired to its own `OcrRouteViewModel` via `OcrTabView`, since the daemon
/// always forces OCR's output to a searchable PDF rather than offering a
/// free format choice. Task 1e.9 moved these five view models' ownership
/// into `ConfigStore` (they used to live directly on this view as
/// `@StateObject`s, seeded once from in-memory defaults and never loaded
/// from disk) so a load/reload can reseed them in place while every tab
/// keeps its existing binding.
struct ContentView: View {
  @StateObject private var store: ConfigStore
  @StateObject private var daemon: DaemonViewModel

  init(store: ConfigStore = ConfigStore(), daemon: DaemonViewModel = DaemonViewModel()) {
    _store = StateObject(wrappedValue: store)
    _daemon = StateObject(wrappedValue: daemon)
  }

  var body: some View {
    VStack(spacing: 0) {
      DaemonStatusBar(state: daemon.state) {
        daemon.refreshState()
      }

      if store.loadState == .missing {
        MissingConfigBanner {
          _ = try? store.createStarterConfigIfNeeded()
        }
      }

      TabView {
        GeneralTabView(viewModel: store.generalViewModel)
          .tabItem {
            Text(Tabs.general.title)
          }
          .tag(Tabs.general)

        RouteEditorView(viewModel: store.fileViewModel)
          .tabItem {
            Text(Tabs.file.title)
          }
          .tag(Tabs.file)

        RouteEditorView(viewModel: store.imageViewModel)
          .tabItem {
            Text(Tabs.image.title)
          }
          .tag(Tabs.image)

        OcrTabView(viewModel: store.ocrViewModel)
          .tabItem {
            Text(Tabs.ocr.title)
          }
          .tag(Tabs.ocr)

        RouteEditorView(viewModel: store.emailViewModel)
          .tabItem {
            Text(Tabs.email.title)
          }
          .tag(Tabs.email)
      }
      .padding()

      SaveBar(
        isDirty: store.isDirty,
        applyOutcome: daemon.lastApplyOutcome,
        onSave: {
          try? store.save()
        },
        onSaveAndApply: {
          daemon.saveAndApply {
            try store.save()
          }
        }
      )
    }
    .frame(minWidth: 480, minHeight: 360)
    .onAppear {
      try? store.load()
      daemon.refreshState()
    }
  }
}

/// The daemon-status line shown above the tabs: `DaemonViewModel.State`'s
/// display text plus a Refresh button, so the user isn't stuck with a
/// stale reading from when the window opened.
private struct DaemonStatusBar: View {
  let state: DaemonViewModel.State
  let onRefresh: () -> Void

  var body: some View {
    HStack {
      Text("Daemon status: \(state.statusText)")
        .foregroundStyle(.secondary)
      Spacer()
      Button("Refresh") {
        onRefresh()
      }
    }
    .padding(8)
  }
}

/// The first-run banner shown atop the window when `ConfigStore.loadState`
/// is `.missing` -- no config file has ever been created at this Mac.
/// `onCreate` calls `ConfigStore.createStarterConfigIfNeeded()`; the actual
/// unsaved-changes/confirmation flow around it is intentionally this thin
/// (see `ConfigStore.isDirty`'s doc comment) -- the decision logic it would
/// gate lives on `ConfigStore`, not here.
private struct MissingConfigBanner: View {
  let onCreate: () -> Void

  var body: some View {
    HStack {
      Text("No configuration file found yet.")
      Spacer()
      Button("Create starter configuration") {
        onCreate()
      }
    }
    .padding(8)
    .background(.yellow.opacity(0.2))
  }
}

/// The Save bar shown under the tabs: an unsaved-changes note, the last
/// `DaemonViewModel.ApplyOutcome`'s guidance (task 1e.10), and both a plain
/// Save button and a "Save & Apply" button that also reloads the running
/// daemon. Both buttons are enabled only when `ConfigStore.isDirty`.
private struct SaveBar: View {
  let isDirty: Bool
  let applyOutcome: DaemonViewModel.ApplyOutcome?
  let onSave: () -> Void
  let onSaveAndApply: () -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      if let applyOutcome {
        Text(applyOutcome.message)
          .font(.caption)
          .foregroundStyle(.secondary)
      }
      HStack {
        if isDirty {
          Text("Unsaved changes")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        Spacer()
        Button("Save") {
          onSave()
        }
        .disabled(!isDirty)

        Button("Save & Apply") {
          onSaveAndApply()
        }
        .disabled(!isDirty)
      }
    }
    .padding([.horizontal, .bottom], 8)
  }
}

#Preview {
  ContentView()
}
