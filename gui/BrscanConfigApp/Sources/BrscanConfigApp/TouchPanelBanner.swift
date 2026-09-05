import SwiftUI

/// The Touch-Panel-override note shown atop every route tab (**File**,
/// **Image**, **OCR**, **Email**): the daemon's own Touch-Panel-ON
/// precedence (task 1d) means a route's config-file defaults only take
/// effect when the destination is started without Touch Panel selections,
/// so the banner says so rather than letting the GUI imply it always
/// controls the scan. Factored out of `RouteEditorView` (task 1e.7) so
/// `OcrTabView` (task 1e.8) shows the exact same wording without a copy of
/// the string literal.
struct TouchPanelBanner: View {
  var body: some View {
    Text(
      "If Touch Panel is on, the printer's own Touch Panel settings for this destination override these defaults."
    )
    .font(.caption)
    .foregroundStyle(.secondary)
  }
}
