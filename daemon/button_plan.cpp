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

// Maps a parsed T= output-type token (Touch-Panel-ON only) to the OCR
// destination's OutputFormat -- the OCR counterpart of FormatForToken. OCR
// only ever produces a searchable PDF (T=PDF(Image), or any image T= the
// panel might report for OCR) or one of the recognized-text sinks
// (T=TXT/HTML/RTF). Any other token falls back to kPdf, a safe,
// always-produceable default, and logs.
OutputFormat OcrFormatForToken(const std::string& output_type) {
  if (output_type == "TXT") return OutputFormat::kText;
  if (output_type == "HTML") return OutputFormat::kHtml;
  if (output_type == "RTF") return OutputFormat::kRtf;
  if (output_type == "PDF(Image)") return OutputFormat::kPdf;
  std::cerr << "[button_plan] unexpected OCR T= token '" << output_type
             << "' from LCD-set config command; defaulting to searchable PDF\n";
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

  // ADF high-speed (X=) follows the same ON/OFF precedence as the scan/output
  // fields below: the LCD's own X= when the panel carried it (Touch-Panel-ON),
  // the daemon's `<dest>.high_speed` config key otherwise (Touch-Panel-OFF).
  // Unlike the other fields it is not a scan Param at all -- it drives a
  // post-scan host-side rotation in HandleButtonEvent (see button_plan.h's
  // ButtonScanPlan::high_speed and daemon/image_transform.h), since the
  // device feeds high-speed pages landscape without transposing the area it
  // reports.
  plan.high_speed = touch_panel_on ? parsed->high_speed
                                    : HighSpeedForFunc(cfg, func);

  // Skip-blank (W=) follows the same ON/OFF precedence: the LCD's own W= when
  // the panel carried it (Touch-Panel-ON), the daemon's `<dest>.skip_blank`
  // config key otherwise (Touch-Panel-OFF). Like high-speed it is not a scan
  // Param -- the device never drops blanks itself (W= is config-only, absent
  // from ESC X; see reference/protocol-notes-button-options.md) -- so it
  // drives a post-scan host-side filter in HandleButtonEvent (see
  // button_plan.h's ButtonScanPlan::skip_blank and daemon/blank_detect.h).
  plan.skip_blank = touch_panel_on ? parsed->skip_blank
                                    : SkipBlankForFunc(cfg, func);

  // Start from this FUNC's configured Params either way, so brightness/
  // contrast/source (none of which the config command carries) come from
  // the daemon's config in both precedence branches.
  plan.params = ParamsForFunc(cfg, func);
  plan.params.button_flow = true;

  // D= (2-sided) always reflects the printer's touch-panel setting, which
  // the panel exposes in both Touch-Panel modes -- both the short "Auto"
  // form and the full LCD-set form carry D=. So it is authoritative
  // regardless of the ON/OFF precedence that governs the other scan/output
  // fields below, unlike E=/edge (not plumbed into Params; the device
  // handles the duplex edge internally -- see
  // reference/protocol-notes-button-options.md), which is left alone.
  plan.params.duplex = parsed->duplex;

  if (touch_panel_on) {
    // Touch-Panel-ON: the printer's own LCD-set settings are authoritative
    // for mode/dpi/area/remove-background (duplex is set above,
    // unconditionally). Unchanged by the `<dest>.remove_background` config
    // key below -- that key is this branch's Touch-Panel-OFF counterpart
    // only (see the `else` branch), never consulted here.
    plan.params.x_dpi = parsed->dpi;
    plan.params.y_dpi = parsed->dpi;
    plan.params.mode = ModeForToken(parsed->mode);
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
    // Touch-Panel-OFF: keep ParamsForFunc's mode/dpi/brightness/contrast as-
    // is (duplex is set above, unconditionally, from the parsed config);
    // only the area is derived here, from the daemon's configured paper for
    // this FUNC, defaulting to kDefaultAutoPaper when none is configured
    // (see its comment above -- Auto mode has no LCD paper size to fall
    // back on).
    std::string paper = PaperForFunc(cfg, func);
    if (paper.empty()) paper = kDefaultAutoPaper;
    if (const std::optional<brscan::Area> area =
            AreaForPaper(paper, plan.params.x_dpi)) {
      plan.params.area = *area;
      // else: leave plan.params.area as ParamsForFunc's own area -- a
      // configured-but-unknown paper token has no captured area to use.
    }

    // Remove-background likewise comes from the daemon's config here --
    // the OFF-path counterpart of the LCD's own G=/L=, which the printer's
    // config command carries (and the ON branch above uses) only when
    // Touch-Panel is ON. ParamsForFunc's own remove_background/_level
    // default (off/0) would otherwise apply unconditionally, since the
    // short "Auto" form never carries G=/L= itself.
    const int remove_background_level = RemoveBackgroundLevelForFunc(cfg, func);
    plan.params.remove_background = remove_background_level != 0;
    plan.params.remove_background_level = remove_background_level;
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

  // OCR's deliverable is a searchable PDF or one of the recognized-text
  // sinks (TXT/HTML/RTF -- see output_writer.h's OutputFormat and
  // action_ocr.h's WriteRecognizedText), chosen by the same ON/OFF
  // precedence the scan/output fields above follow:
  //   - Touch-Panel-ON: from the LCD's own T= sub-format (parsed->
  //     output_type) via OcrFormatForToken.
  //   - Touch-Panel-OFF: from the daemon's `ocr.ocr_format` config key
  //     (Config::ocr_text_format), which defaults to kPdf.
  // `searchable` is set true only for kPdf -- it lays the Vision text layer
  // into a PDF page, and means nothing for the text sinks (which are
  // themselves the recognized text). This is the promotion
  // daemon/handle_event.cpp used to do inline; it now lives here so every
  // button-scan path goes through it.
  if (func == kFuncOcr) {
    // Start from the daemon's configured OCR output settings (so a
    // configured separation/separate_n/tiff_compression survive), then
    // override the format per precedence and set searchable to match.
    plan.output = OutputSettingsForFunc(cfg, func);
    plan.output.format = touch_panel_on ? OcrFormatForToken(parsed->output_type)
                                        : cfg.ocr_text_format;
    plan.output.searchable = plan.output.format == OutputFormat::kPdf;
  } else if (touch_panel_on) {
    plan.output.format = FormatForToken(parsed->output_type);
    plan.output.tiff_compression = TiffCompression::kLzw;
    plan.output.separation = OutputSeparation::kCombine;
    plan.output.searchable = false;
  } else {
    plan.output = OutputSettingsForFunc(cfg, func);
  }

  return plan;
}

}  // namespace brscan::scand
