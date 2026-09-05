import SwiftUI

/// The config window's content: a five-tab `TabView` keyed by `Tabs`.
///
/// The **General** tab (task 1e.6) is wired to a real `GeneralViewModel`;
/// **File** / **Image** / **Email** (task 1e.7) are each wired to their own
/// `RouteViewModel` via the shared `RouteEditorView`. **OCR** still hosts a
/// placeholder `Form` -- that wiring, plus the Vision OCR-only `searchable`
/// field, is task 1e.8.
struct ContentView: View {
  @StateObject private var generalViewModel = GeneralViewModel()
  @StateObject private var fileViewModel = RouteViewModel()
  @StateObject private var imageViewModel = RouteViewModel()
  @StateObject private var emailViewModel = RouteViewModel()

  var body: some View {
    TabView {
      GeneralTabView(viewModel: generalViewModel)
        .tabItem {
          Text(Tabs.general.title)
        }
        .tag(Tabs.general)

      RouteEditorView(viewModel: fileViewModel)
        .tabItem {
          Text(Tabs.file.title)
        }
        .tag(Tabs.file)

      RouteEditorView(viewModel: imageViewModel)
        .tabItem {
          Text(Tabs.image.title)
        }
        .tag(Tabs.image)

      PlaceholderForm(tab: .ocr)
        .tabItem {
          Text(Tabs.ocr.title)
        }
        .tag(Tabs.ocr)

      RouteEditorView(viewModel: emailViewModel)
        .tabItem {
          Text(Tabs.email.title)
        }
        .tag(Tabs.email)
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
