import SwiftUI

/// The config window's content: a five-tab `TabView` keyed by `Tabs`.
///
/// The **General** tab (task 1e.6) is wired to a real `GeneralViewModel`;
/// **File** / **Image** / **OCR** / **Email** still host a placeholder
/// `Form` -- static labels and disabled controls only, no bindings to
/// `BrscanConfigCore` yet. That wiring is Tasks 1e.7-1e.8.
struct ContentView: View {
  @StateObject private var generalViewModel = GeneralViewModel()

  var body: some View {
    TabView {
      GeneralTabView(viewModel: generalViewModel)
        .tabItem {
          Text(Tabs.general.title)
        }
        .tag(Tabs.general)

      ForEach(Tabs.allCases.filter { $0 != .general }) { tab in
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
