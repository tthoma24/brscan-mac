import BrscanConfigCore
import SwiftUI

/// A reusable route editor `Form`, bound to a `RouteViewModel`. Wired into
/// the **File**, **Image**, and **Email** tabs (task 1e.7), each holding
/// its own `RouteViewModel` -- **OCR** gets its own editor in task 1e.8.
///
/// Every picker's option list comes straight from `OptionValueSets` (task
/// 1e.3), so this view never offers a token the daemon would reject.
/// `tiff_compression` and the separation controls are disabled per
/// `RouteViewModel.isCompressionEditable`/`isSeparationEditable`, which
/// mirror `OptionRules`'s format-gating; labels follow the Google
/// Developer Style Guide's sentence-case guidance for UI text.
struct RouteEditorView: View {
  @ObservedObject var viewModel: RouteViewModel

  var body: some View {
    Form {
      Section {
        // Non-editable info row: the daemon's own Touch-Panel-ON
        // precedence (task 1d) means these defaults only take effect when
        // the destination is started without Touch Panel selections, so
        // the banner says so rather than letting the GUI imply it always
        // controls the scan.
        Text(
          "If Touch Panel is on, the printer's own Touch Panel settings for this destination override these defaults."
        )
        .font(.caption)
        .foregroundStyle(.secondary)
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
        Picker("Format", selection: $viewModel.format) {
          ForEach(OptionValueSets.format.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }

        Picker("TIFF compression", selection: $viewModel.tiffCompression) {
          ForEach(OptionValueSets.tiffCompression.options, id: \.self) { option in
            Text(option).tag(option)
          }
        }
        .disabled(!viewModel.isCompressionEditable)
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

        if !viewModel.isSeparationEditable {
          Text("Not applicable for this format -- each page is already written to its own file.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
      }
    }
  }
}

#Preview {
  RouteEditorView(viewModel: RouteViewModel())
}
