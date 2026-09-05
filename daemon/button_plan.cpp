#include "button_plan.h"

#include <iostream>

#include "button_config.h"
#include "paper_size.h"

namespace brscan::scand {

// Touch-Panel-OFF ("Auto") config commands carry no LCD paper size, and the
// ADF's ESC I offer can't report sheet length ahead of the feed (its ymax
// is always 0) -- so a still-zero scan area at the end of PlanButtonScan
// would otherwise reach RunButtonScan's offer fallback and produce a
// zero-height scan area (protocol error on every Auto-mode press). This is
// the sensible default the daemon falls back to instead; a user can
// override it per FUNC with `<dest>.paper`.
constexpr char kDefaultAutoPaper[] = "LETTER";

namespace {

// Maps a parsed M= token to a ScanMode. Any other token (not expected from
// the LCD panel -- see button_config.h's ButtonConfig::mode) keeps kColor
// and logs, rather than failing the whole plan over one unrecognized
// field.
brscan::ScanMode ModeForToken(const std::string& mode_token) {
  if (mode_token == "TEXT") return brscan::ScanMode::kBlackWhite;
  if (mode_token != "CGRAY") {
    std::cerr << "[button_plan] unexpected M= token '" << mode_token
               << "' from LCD-set config command; defaulting to color\n";
  }
  return brscan::ScanMode::kColor;
}

// Maps a parsed T= output-type token (Touch-Panel-ON only) to an
// OutputFormat. Any other token (including the OCR-only TXT/HTML/RTF sub-
// formats -- see PlanButtonScan's deferred-fields note below) falls back
// to kPdf, a safe, always-produceable default, and logs.
OutputFormat FormatForToken(const std::string& output_type) {
  if (output_type == "PDF(Image)") return OutputFormat::kPdf;
  if (output_type == "MULTI-TIFF") return OutputFormat::kTiff;
  if (output_type == "JPEG") return OutputFormat::kJpeg;
  std::cerr << "[button_plan] unexpected T= token '" << output_type
             << "' from LCD-set config command; defaulting to PDF\n";
  return OutputFormat::kPdf;
}

}  // namespace

std::optional<ButtonScanPlan> PlanButtonScan(
    const std::vector<uint8_t>& config_frame, const std::string& func,
    const Config& cfg) {
  const std::optional<ButtonConfig> parsed =
      ParseButtonConfig(config_frame.data(), config_frame.size());
  if (!parsed) return std::nullopt;

  // Touch-Panel-ON detection: the short "Auto" form carries only F=/D=/E=
  // (no R=), so ParseButtonConfig's dpi field is left at its 0 default;
  // every explicit LCD-set form carries R= with a positive value. This is
  // the one signal this project distinguishes the two forms by -- see
  // button_plan.h's header comment for the full rationale.
  const bool touch_panel_on = parsed->dpi > 0;

  ButtonScanPlan plan;
  plan.touch_panel_on = touch_panel_on;

  // Start from this FUNC's configured Params either way, so brightness/
  // contrast/source (none of which the config command carries) come from
  // the daemon's config in both precedence branches.
  plan.params = ParamsForFunc(cfg, func);
  plan.params.button_flow = true;

  if (touch_panel_on) {
    // Touch-Panel-ON: the printer's own LCD-set settings are authoritative
    // for mode/dpi/duplex/area/remove-background.
    plan.params.x_dpi = parsed->dpi;
    plan.params.y_dpi = parsed->dpi;
    plan.params.mode = ModeForToken(parsed->mode);
    plan.params.duplex = parsed->duplex;
    plan.params.remove_background = parsed->remove_background;
    plan.params.remove_background_level = parsed->remove_background_level;

    const std::optional<brscan::Area> area =
        AreaForPaper(parsed->paper, parsed->dpi);
    if (area) {
      plan.params.area = *area;
    } else {
      // Unknown/empty paper token: leave the area at the zero value so
      // RunButtonScan falls back to the ESC I offer's full area (see
      // brscan/scanner.h's RunButtonScan doc comment).
      plan.params.area = brscan::Area{0, 0, 0, 0};
      std::cerr << "[button_plan] unknown P= token '" << parsed->paper
                 << "' from LCD-set config command; requesting the ESC I "
                    "offer's full area instead\n";
    }
  } else {
    // Touch-Panel-OFF: keep ParamsForFunc's mode/dpi/duplex/brightness/
    // contrast as-is; only the area is derived here, from the daemon's
    // configured paper for this FUNC, defaulting to kDefaultAutoPaper when
    // none is configured (see its comment above -- Auto mode has no LCD
    // paper size to fall back on).
    std::string paper = PaperForFunc(cfg, func);
    if (paper.empty()) paper = kDefaultAutoPaper;
    if (const std::optional<brscan::Area> area =
            AreaForPaper(paper, plan.params.x_dpi)) {
      plan.params.area = *area;
      // else: leave plan.params.area as ParamsForFunc's own area -- a
      // configured-but-unknown paper token has no captured area to use.
    }
  }

  // Final safety net, both branches: if the area is still the zero value
  // here, either Touch-Panel-OFF's configured (but unknown) paper token or
  // Touch-Panel-ON's LCD-supplied (but unknown) P= token left it that way.
  // Never let a zero area reach RunButtonScan, which would otherwise fall
  // back to the ADF ESC I offer's ymax=0 and produce a zero-height scan
  // area -- fall back to kDefaultAutoPaper's concrete area instead.
  const brscan::Area& final_area = plan.params.area;
  if (final_area.x0 == 0 && final_area.y0 == 0 && final_area.x1 == 0 &&
      final_area.y1 == 0) {
    if (const std::optional<brscan::Area> default_area =
            AreaForPaper(kDefaultAutoPaper, plan.params.x_dpi)) {
      plan.params.area = *default_area;
    }
  }

  // OCR's deliverable is always a searchable PDF, in both precedence
  // branches: "native" wouldn't be searchable, and `searchable` only ever
  // means anything for a PDF page (see output_writer.h). This is the
  // promotion daemon/handle_event.cpp used to do inline; it now lives here
  // so every button-scan path (Touch-Panel-ON or -OFF) goes through it.
  //
  // TODO (Plan 1d follow-on): the LCD's OCR sub-formats (T=TXT/HTML/RTF --
  // see button_config.h's ButtonConfig::output_type) are parsed but not
  // yet acted on; OCR always produces a searchable PDF regardless of
  // which of those three the panel offered.
  if (func == kFuncOcr) {
    // Start from the daemon's configured OCR output settings (so a
    // configured separation/separate_n/tiff_compression survive), then
    // override only the two fields OCR's deliverable always forces.
    plan.output = OutputSettingsForFunc(cfg, func);
    plan.output.format = OutputFormat::kPdf;
    plan.output.searchable = true;
  } else if (touch_panel_on) {
    plan.output.format = FormatForToken(parsed->output_type);
    plan.output.tiff_compression = TiffCompression::kLzw;
    plan.output.separation = OutputSeparation::kCombine;
    plan.output.searchable = false;
  } else {
    plan.output = OutputSettingsForFunc(cfg, func);
  }

  // TODO (Plan 1d follow-on): parsed->skip_blank (W=) and
  // parsed->high_speed (X=) are decoded by ParseButtonConfig but not yet
  // acted on here -- host-side blank-page skipping and the LCD's
  // high-speed/rotation (landscape) mode are both deferred to a later
  // task (see docs/BUTTON.md).

  return plan;
}

}  // namespace brscan::scand
