import SwiftUI

/// The config window's content: a five-tab `TabView` keyed by `Tabs`.
///
/// The **General** tab (task 1e.6) is wired to a real `GeneralViewModel`;
/// **File** / **Image** / **Email** (task 1e.7) are each wired to their own
/// `RouteViewModel` via the shared `RouteEditorView`. **OCR** (task 1e.8) is
/// wired to its own `OcrRouteViewModel` via `OcrTabView`, since the daemon
/// always forces OCR's output to a searchable PDF rather than offering a
/// free format choice.
struct ContentView: View {
  @StateObject private var generalViewModel = GeneralViewModel()
  @StateObject private var fileViewModel = RouteViewModel()
  @StateObject private var imageViewModel = RouteViewModel()
  @StateObject private var ocrViewModel = OcrRouteViewModel()
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

      OcrTabView(viewModel: ocrViewModel)
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

#Preview {
  ContentView()
}
