import BrscanConfigCore
import SwiftUI

/// The **OCR** tab's editor `Form`, bound to an `OcrRouteViewModel` (task
/// 1e.8) -- a specialization of `RouteEditorView` (task 1e.7) for how the
/// daemon actually treats OCR output: always a searchable PDF, never a free
/// format choice. So this view shows scan params (mode/source/dpi/paper)
/// and separation exactly like `RouteEditorView`, but its "Output format"
/// section has no format picker and no `tiff_compression` control -- just a
/// non-editable "Output: Searchable PDF" row plus notes (Google Developer
/// Style Guide sentence case) explaining why, and the same Touch-Panel
/// banner every route tab shows.
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
        LabeledContent("Output", value: "Searchable PDF")

        Text(OcrRouteViewModel.Notes.searchablePdf)
          .font(.caption)
          .foregroundStyle(.secondary)

        Text(OcrRouteViewModel.Notes.subFormatsNotProduced)
          .font(.caption)
          .foregroundStyle(.secondary)
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
