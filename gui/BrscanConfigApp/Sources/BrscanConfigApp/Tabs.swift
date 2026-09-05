import Foundation

/// The five top-level sections of the config window, in tab order.
///
/// This enum is the single source of truth for the `TabView`'s tab
/// identifiers and titles (see `ContentView.swift`). Task 1e.5 defines only
/// the shell -- each case's view is a static placeholder `Form`; real field
/// bindings to `BrscanConfigCore` arrive in Tasks 1e.6-1e.8.
public enum Tabs: String, CaseIterable, Identifiable {
  case general
  case file
  case image
  case ocr
  case email

  public var id: String { rawValue }

  /// The tab's display title, title-cased per the Google Developer Style
  /// Guide's guidance for UI labels.
  public var title: String {
    switch self {
    case .general: return "General"
    case .file: return "File"
    case .image: return "Image"
    case .ocr: return "OCR"
    case .email: return "Email"
    }
  }
}
