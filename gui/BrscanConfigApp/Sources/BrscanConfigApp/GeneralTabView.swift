import SwiftUI

/// The **General** tab's real form, bound to a `GeneralViewModel`. Field
/// order and labels follow `docs/PLAN-1E-DESIGN.md`'s "General (machine-wide
/// keys)" table; labels are sentence-case per the Google Developer Style
/// Guide's guidance for UI text.
///
/// This establishes the field-layout pattern (label, optional hint/
/// validation note, one control per config key) task 1e.7's route tabs
/// reuse for `DaemonConfig.Route`'s fields.
struct GeneralTabView: View {
  @ObservedObject var viewModel: GeneralViewModel

  var body: some View {
    Form {
      Section("General") {
        VStack(alignment: .leading, spacing: 4) {
          TextField("Printer host", text: $viewModel.printerHost)
          if viewModel.isPrinterHostMissing {
            // Clean-room per-task rule: no real device identity, so the
            // discovery hint below names no host of its own -- just the
            // `dns-sd` invocation from `config/brscan-scand.conf.example`.
            Text("Required. Find your printer's host with `dns-sd -B _scanner._tcp` on its network.")
              .font(.caption)
              .foregroundStyle(.red)
          }
        }

        TextField("Display name", text: $viewModel.displayName)

        HStack {
          TextField("Save folder", text: $viewModel.saveDir)
          Button("Choose…") {
            viewModel.pickSaveDir()
          }
        }

        TextField("Image opens with", text: $viewModel.imageApp)

        TextField("Email to", text: $viewModel.emailTo)
      }
    }
  }
}

#Preview {
  GeneralTabView(viewModel: GeneralViewModel())
}
