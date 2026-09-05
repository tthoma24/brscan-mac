import BrscanConfigCore
import Combine
import Foundation

/// App-level coordinator (task 1e.9) that loads and saves
/// `~/.config/brscan-scand.conf`, seeding the five tabs' view models from it
/// and, on save, folding their current values back into the config file
/// -- preserving every comment, blank line, key order, and unrecognized key
/// the app doesn't model, via `BrscanConfigCore`'s `ConfigDocument`/
/// `DaemonConfig` (tasks 1e.2/1e.4).
///
/// All file access goes through the injected `ConfigFileSystem` seam, so
/// tests never touch the real `~/.config` directory.
public final class ConfigStore: ObservableObject {

  /// Whether the config file was found the last time `load()` ran.
  public enum LoadState: Equatable {
    /// `load()` hasn't run yet.
    case notLoaded
    /// `load()` ran, but the file didn't exist at `configURL` -- see
    /// `createStarterConfigIfNeeded()`, the "first run" affordance.
    case missing
    /// `load()` ran and successfully seeded every tab's view model.
    case loaded
  }

  /// The five tabs' view models, seeded by `load()`/`createStarterConfigIfNeeded()`
  /// and read back by `save()`. Owned here (rather than by `ContentView`, as
  /// before this task) so the same instances survive a reload -- SwiftUI's
  /// bindings stay attached to the same objects; only their published
  /// fields change.
  public let generalViewModel: GeneralViewModel
  public let fileViewModel: RouteViewModel
  public let imageViewModel: RouteViewModel
  public let ocrViewModel: OcrRouteViewModel
  public let emailViewModel: RouteViewModel

  @Published public private(set) var loadState: LoadState = .notLoaded

  private let fileSystem: ConfigFileSystem
  private let configURL: URL

  /// The document `load()` last read (or `createStarterConfigIfNeeded()`
  /// last wrote), held so `save()` can fold edits back onto it without
  /// disturbing anything the app's view models don't own -- comments, blank
  /// lines, key order, unrecognized keys. Starts as an empty document so a
  /// `save()` before any successful `load()` still produces a well-formed
  /// file (all five view models' defaults, nothing else).
  private var document = ConfigDocument()

  /// The `DaemonConfig` last loaded from, or saved to, disk -- `isDirty`
  /// compares the view models' current values against this snapshot.
  private var lastSavedConfig = DaemonConfig.default

  /// Subscriptions forwarding each child view model's `objectWillChange` to
  /// this store's own, so a SwiftUI view reading `isDirty` off `ConfigStore`
  /// re-renders whenever any tab's field changes -- not just when
  /// `ConfigStore`'s own `@Published` properties change.
  private var childCancellables: [AnyCancellable] = []

  /// The header comment written atop a freshly created starter config (see
  /// `createStarterConfigIfNeeded()`). Google Developer Style Guide
  /// sentence case.
  private static let starterConfigHeader = """
    # brscan-scand configuration, created by the Brscan Config app.
    # Edit the values below, or use the app's tabs instead.

    """

  /// `~/.config/brscan-scand.conf`, with `~` expanded to the current user's
  /// home directory.
  public static var defaultConfigURL: URL {
    FileManager.default.homeDirectoryForCurrentUser
      .appendingPathComponent(".config", isDirectory: true)
      .appendingPathComponent("brscan-scand.conf", isDirectory: false)
  }

  public init(
    fileSystem: ConfigFileSystem = LocalConfigFileSystem(),
    configURL: URL = ConfigStore.defaultConfigURL
  ) {
    self.fileSystem = fileSystem
    self.configURL = configURL
    self.generalViewModel = GeneralViewModel()
    self.fileViewModel = RouteViewModel()
    self.imageViewModel = RouteViewModel()
    self.ocrViewModel = OcrRouteViewModel()
    self.emailViewModel = RouteViewModel()

    observeChildViewModelsForDirtyTracking()
  }

  private func observeChildViewModelsForDirtyTracking() {
    let publishers: [AnyPublisher<Void, Never>] = [
      generalViewModel.objectWillChange.eraseToAnyPublisher(),
      fileViewModel.objectWillChange.eraseToAnyPublisher(),
      imageViewModel.objectWillChange.eraseToAnyPublisher(),
      ocrViewModel.objectWillChange.eraseToAnyPublisher(),
      emailViewModel.objectWillChange.eraseToAnyPublisher(),
    ]
    childCancellables = publishers.map { publisher in
      publisher.sink { [weak self] in
        self?.objectWillChange.send()
      }
    }
  }

  // MARK: Dirty state

  /// The config the five view models' current values would produce, if
  /// saved right now.
  private var currentConfig: DaemonConfig {
    DaemonConfig(
      general: generalViewModel.general,
      file: fileViewModel.route,
      image: imageViewModel.route,
      ocr: ocrViewModel.route,
      email: emailViewModel.route)
  }

  /// `true` when any tab's view model differs from the config last loaded
  /// or saved -- backs a Save button's enabled state and an unsaved-changes
  /// prompt's decision (the prompt itself is a thin view concern; this is
  /// the testable logic behind it). Cleared by a successful `save()`;
  /// reverting a field back to its loaded/saved value also clears it, since
  /// this compares whole-config equality rather than tracking "touched".
  public var isDirty: Bool {
    currentConfig != lastSavedConfig
  }

  // MARK: Load

  /// Whether a config file currently exists at `configURL` -- backs the
  /// "first run" affordance's visibility (only offered when there is
  /// nothing to overwrite).
  public var configFileExists: Bool {
    fileSystem.fileExists(at: configURL)
  }

  /// Reads `configURL` and seeds every tab's view model from it. If the
  /// file doesn't exist yet, sets `loadState` to `.missing` and leaves the
  /// view models untouched (at whatever they already held) rather than
  /// throwing -- "no config file yet" is an expected first-run state, not
  /// an error; see `createStarterConfigIfNeeded()`.
  public func load() throws {
    guard fileSystem.fileExists(at: configURL) else {
      loadState = .missing
      return
    }
    let text = try fileSystem.contents(of: configURL)
    applyLoadedDocument(ConfigDocument(text: text))
    loadState = .loaded
  }

  private func applyLoadedDocument(_ doc: ConfigDocument) {
    document = doc
    let config = DaemonConfig.from(doc)
    generalViewModel.load(config.general)
    fileViewModel.load(config.file)
    imageViewModel.load(config.image)
    ocrViewModel.load(config.ocr)
    emailViewModel.load(config.email)

    // The dirty-state snapshot is taken from the view models' own
    // reassembled values (`currentConfig`), not the raw parsed `config`:
    // `OcrRouteViewModel.route` always forces `format == "pdf"` regardless
    // of what was on disk (see that type's doc comment), so a file whose
    // `ocr.format` was something else would otherwise look "dirty" the
    // instant it's loaded, before any edit has happened.
    lastSavedConfig = currentConfig
  }

  // MARK: Save

  /// Applies every tab's current values onto the held `ConfigDocument` --
  /// only the keys `DaemonConfig` owns (`DaemonConfig.apply(to:)`, task
  /// 1e.4), so comments, blank lines, key order, and any key this app
  /// doesn't model are left byte-for-byte untouched -- then writes the
  /// result to `configURL` atomically (a temp file in the same directory,
  /// written first, then renamed into place; see `ConfigFileSystem`).
  /// Clears `isDirty` on success.
  public func save() throws {
    let config = currentConfig
    var doc = document
    config.apply(to: &doc)

    try writeAtomically(doc.serialized(), to: configURL)

    document = doc
    lastSavedConfig = config
  }

  private func writeAtomically(_ text: String, to url: URL) throws {
    let directory = url.deletingLastPathComponent()
    let tempURL = directory.appendingPathComponent(".\(url.lastPathComponent).tmp-\(UUID().uuidString)")
    try fileSystem.write(text, to: tempURL)
    try fileSystem.moveItem(at: tempURL, to: url)
  }

  // MARK: First run / missing file

  /// Writes a starter config -- `DaemonConfig.default`'s values, serialized
  /// through a fresh `ConfigDocument` with a short header comment -- to
  /// `configURL`, then loads it so the view models immediately reflect it.
  /// Never overwrites an existing file: a no-op (returning `false`, without
  /// touching disk) if `configURL` already exists. This is the "first run"
  /// affordance, not a reset-to-defaults action.
  @discardableResult
  public func createStarterConfigIfNeeded() throws -> Bool {
    guard !fileSystem.fileExists(at: configURL) else { return false }

    var doc = ConfigDocument(text: Self.starterConfigHeader)
    DaemonConfig.default.apply(to: &doc)
    try writeAtomically(doc.serialized(), to: configURL)

    try load()
    return true
  }
}
