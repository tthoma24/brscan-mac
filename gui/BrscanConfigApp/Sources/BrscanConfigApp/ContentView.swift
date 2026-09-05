import SwiftUI

/// The config window's content: a five-tab `TabView` keyed by `Tabs`.
///
/// Every tab hosts a placeholder `Form` for now -- static labels and
/// disabled controls only, no bindings to `BrscanConfigCore` yet. That
/// wiring is Tasks 1e.6-1e.8; this task is the navigation shell only.
struct ContentView: View {
  var body: some View {
    TabView {
      ForEach(Tabs.allCases) { tab in
        PlaceholderForm(tab: tab)
          .tabItem {
            Text(tab.title)
          }
          .tag(tab)
      }
    }
    .padding()
    .frame(minWidth: 480, minHeight: 360)
  }
}

/// A static placeholder body for one tab. No real fields yet -- just enough
/// structure to show where each tab's controls will eventually live.
private struct PlaceholderForm: View {
  let tab: Tabs

  var body: some View {
    Form {
      Section(tab.title) {
        Text("\(tab.title) settings placeholder.")
          .foregroundStyle(.secondary)
        Toggle("Placeholder option", isOn: .constant(false))
          .disabled(true)
      }
    }
  }
}

#Preview {
  ContentView()
}
