import BrscanConfigCore
import SwiftUI

/// The **OCR** tab's editor `Form`, bound to an `OcrRouteViewModel` (task
/// 1e.8) -- a specialization of `RouteEditorView` (task 1e.7) for how the
/// daemon treats OCR output. So this view shows scan params
/// (mode/source/dpi/paper) and separation exactly like `RouteEditorView`,
/// but its "Output format" section offers the OCR-specific sub-format picker
/// (`OptionValueSets.ocrFormat`: pdf/txt/html/rtf, the daemon's
/// `ocr.ocr_format` key) instead of the image-format + `tiff_compression`
/// controls -- with a searchable-PDF note shown only for the `pdf` choice --
/// plus the same Touch-Panel banner every route tab shows.
struct OcrTabView: View {
  @ObservedObject var viewModel: OcrRouteViewModel

  var body: some View {
    Form {
      Section {
        TouchPanelBanner()
      }

      Section("Scan settings") {
        Picker("Mode", selection: $viewModel.mode) {
          ForEach(OptionValueSets.mode.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }

        Picker("Source", selection: $viewModel.source) {
          ForEach(OptionValueSets.source.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }

        VStack(alignment: .leading, spacing: 4) {
          Stepper("Resolution: \(viewModel.dpi) dpi", value: $viewModel.dpi, in: 1...9999)
          if !viewModel.isDpiValid {
            Text("Resolution must be a positive number.")
              .font(.caption)
              .foregroundStyle(.red)
          }
        }

        Picker("Paper", selection: $viewModel.paper) {
          Text("Unspecified").tag("")
          ForEach(OptionValueSets.paper.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }
      }

      Section("Output format") {
        Picker("Output", selection: $viewModel.format) {
          ForEach(OptionValueSets.ocrFormat.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }

        if viewModel.isSearchablePdf {
          Text(OcrRouteViewModel.Notes.searchablePdf)
            .font(.caption)
            .foregroundStyle(.secondary)
        }
      }

      Section("Multi-page output") {
        Picker("Split output files", selection: $viewModel.separationMode) {
          ForEach(RouteViewModel.SeparationMode.allCases) { mode in
            Text(mode.title).tag(mode)
          }
        }
        .disabled(!viewModel.isSeparationEditable)

        if viewModel.separationMode != .combine {
          VStack(alignment: .leading, spacing: 4) {
            Stepper(
              "Every \(viewModel.separationCount) \(viewModel.separationMode == .image ? "images" : "pages")",
              value: $viewModel.separationCount, in: 1...999
            )
            .disabled(!viewModel.isSeparationEditable)
            if !viewModel.isSeparationCountValid {
              Text("Count must be a positive number.")
                .font(.caption)
                .foregroundStyle(.red)
            }
          }
        }
      }
    }
  }
}

#Preview {
  OcrTabView(viewModel: OcrRouteViewModel())
}
