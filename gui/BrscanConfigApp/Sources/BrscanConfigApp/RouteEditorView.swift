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

        Toggle("High-speed ADF (rotate to portrait)", isOn: $viewModel.highSpeed)

        Stepper("Resolution: \(viewModel.dpi) dpi", value: $viewModel.dpi, in: 1...9999)

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

        // JPEG-only "File size" slider, mirroring the Brother driver's
        // "File Size: Small ... High Quality" control. Shown only when the
        // selected format is jpeg (see RouteViewModel.isJpegQualityEditable),
        // the same way tiff_compression is gated on tiff above.
        if viewModel.isJpegQualityEditable {
          VStack(alignment: .leading, spacing: 4) {
            Slider(
              value: Binding(
                get: { Double(viewModel.jpegQuality) },
                set: { viewModel.jpegQuality = Int($0.rounded()) }),
              in: Double(JpegQuality.range.lowerBound)...Double(JpegQuality.range.upperBound),
              step: 1
            ) {
              Text("File size")
            } minimumValueLabel: {
              Text("Small")
            } maximumValueLabel: {
              Text("High Quality")
            }
            Text("Quality: \(viewModel.jpegQuality)")
              .font(.caption)
              .foregroundStyle(.secondary)
          }
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
          Stepper(
            "Every \(viewModel.separationCount) \(viewModel.separationMode == .image ? "images" : "pages")",
            value: $viewModel.separationCount, in: 1...999
          )
          .disabled(!viewModel.isSeparationEditable)
        }

        if !viewModel.isSeparationEditable {
          Text("Not applicable for this format -- each page is already written to its own file.")
            .font(.caption)
            .foregroundStyle(.secondary)
        }
      }

      Section("Page handling") {
        Toggle("Skip blank pages", isOn: $viewModel.skipBlank)
      }
    }
  }
}

#Preview {
  RouteEditorView(viewModel: RouteViewModel())
}
