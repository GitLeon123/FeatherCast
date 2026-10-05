// Included inside FeatherCastApp: Library editing, persistence and reload coordination.
// Window ownership and async event queues remain with the app; pure validation lives
// in library.cpp and automation.hpp.

  feathercast::library_ui::ManagerData LibraryManagerData() {
    feathercast::library_ui::ManagerData data;
    {
      std::lock_guard lock(dataMutex_);
      data.snippets = snippets_;
      for (const auto& app : apps_) {
        if (!app.id.empty() && !app.name.empty()) {
          data.availableApps.push_back({app.id, app.name});
        }
      }
    }
    data.quicklinks = settings_.quicklinks;
    data.scripts = settings_.scripts;
    data.workspaces = settings_.workspaces;
    for (const auto& [appId, alias] : settings_.appAliases) {
      const auto app = std::find_if(
          data.availableApps.begin(), data.availableApps.end(),
          [&](const auto& choice) { return choice.id == appId; });
      data.appAliases.push_back(
          {appId, app == data.availableApps.end() ? L"" : app->name, alias});
    }
    for (const auto& [keyword, urlTemplate] : settings_.searchEngines) {
      data.webSearches.push_back({keyword, urlTemplate});
    }
    std::vector<feathercast::library::CommandChoice> commandChoices;
    for (const auto& command : feathercast::commands::Catalog()) {
      commandChoices.push_back({std::wstring(command.stableId), std::wstring(command.label)});
    }
    data.commandAliases = feathercast::library::BuildCommandAliases(settings_.commandAliases, commandChoices);
    data.commandShortcuts = feathercast::library::BuildCommandAliases(settings_.commandShortcuts, commandChoices);
    std::sort(data.availableApps.begin(), data.availableApps.end(),
              [](const auto& left, const auto& right) {
                return feathercast::library::NormalizeKeyword(left.name) <
                       feathercast::library::NormalizeKeyword(right.name);
              });
    data.snippetsWritable = snippetsWritable_;
    data.quicklinksWritable = !settingsPersistenceBlocked_;
    data.settingsWritable = !settingsPersistenceBlocked_;
    data.snippetsMessage = snippetsLoadMessage_;
    if (settingsPersistenceBlocked_) {
      data.quicklinksMessage =
          L"settings.json is protected from automatic writes. Quicklink editing is disabled.";
      data.settingsMessage =
          L"settings.json is protected from automatic writes. Customization is disabled.";
    }
    return data;
  }

  bool StartNextSnippetSave() {
    if (snippetSaveInFlight_ || !pendingSnippetSave_) return true;
    PendingSnippetSave pending = std::move(*pendingSnippetSave_);
    pendingSnippetSave_.reset();
    const auto expected = snippetsFingerprint_;
    const auto path = SnippetsPath();
    snippetSaveInFlight_ = true;
    if (!launchExecutor_.Submit(
            [this, path, expected,
             snippets = std::move(pending.snippets)](std::stop_token stopToken) {
              if (stopToken.stop_requested()) return;
              feathercast::snippets_io::SaveResult saved;
              {
                std::lock_guard lock(snippetsIoMutex_);
                saved = feathercast::snippets_io::Save(path, snippets, expected);
              }
              if (!stopToken.stop_requested()) {
                snippetSaveEvents_.Push(SnippetSaveCompleted{std::move(saved)});
              }
            })) {
      snippetSaveInFlight_ = false;
      return false;
    }
    return true;
  }

  bool QueueSnippetSave(
      const std::vector<feathercast::snippets::Snippet>& candidate) {
    ++snippetSaveGeneration_;
    pendingSnippetSave_ = PendingSnippetSave{candidate};
    return StartNextSnippetSave();
  }

  bool SnippetSaveActive() const {
    return snippetSaveInFlight_ || pendingSnippetSave_.has_value();
  }

  bool QueueSnippetReload() {
    // Saves and reloads can run on different workers, so a reload waits for
    // queued saves; otherwise it could read the file before they write it.
    if (SnippetSaveActive()) {
      snippetReloadAfterSave_ = true;
      return true;
    }
    if (snippetReloadPending_) return true;
    snippetReloadPending_ = true;
    const auto path = SnippetsPath();
    const std::uint64_t saveGeneration = snippetSaveGeneration_;
    if (!launchExecutor_.Submit([this, path, saveGeneration](
                                    std::stop_token stopToken) {
          if (stopToken.stop_requested()) return;
          feathercast::snippets_io::LoadResult loaded;
          {
            std::lock_guard lock(snippetsIoMutex_);
            loaded = feathercast::snippets_io::Load(path);
          }
          if (!stopToken.stop_requested()) {
            snippetLoadEvents_.Push(
                SnippetLoadCompleted{saveGeneration, std::move(loaded)});
          }
        })) {
      snippetReloadPending_ = false;
      return false;
    }
    return true;
  }

  void HandleSnippetSaveCompleted(SnippetSaveCompleted completion) {
    snippetSaveInFlight_ = false;
    if (completion.result.succeeded) {
      snippetsFingerprint_ = completion.result.fingerprint;
      snippetsWritable_ = true;
      snippetsLoadMessage_.clear();
      if (!pendingSnippetSave_ && settingsStatus_ &&
          settingsStatus_->severity == StatusSeverity::Progress &&
          settingsStatus_->text == L"Saving snippets...") {
        SetSettingsStatus(StatusSeverity::Success, L"Snippets saved.");
      }
      if (!StartNextSnippetSave()) {
        ReportPersistenceFailure(L"The snippets worker is unavailable.");
      }
    } else {
      pendingSnippetSave_.reset();
      snippetsFingerprint_ = completion.result.fingerprint;
      snippetsWritable_ = false;
      snippetsLoadMessage_ = completion.result.message;
      ReportPersistenceFailure(
          completion.result.message.empty()
              ? L"Snippets could not be saved."
              : completion.result.message);
    }
    if (snippetReloadAfterSave_ && !SnippetSaveActive()) {
      snippetReloadAfterSave_ = false;
      if (!QueueSnippetReload()) {
        ReportPersistenceFailure(L"The snippets worker is unavailable.");
      }
    }
  }

  void HandleSnippetLoadCompleted(SnippetLoadCompleted completion) {
    snippetReloadPending_ = false;
    // A save queued after this reload holds newer data than the file did.
    if (completion.saveGeneration != snippetSaveGeneration_ ||
        SnippetSaveActive()) {
      return;
    }
    ApplySnippetReload(std::move(completion.result));
  }

  void ApplySnippetReload(feathercast::snippets_io::LoadResult loaded) {
    snippetsFingerprint_ = loaded.fingerprint;
    snippetsWritable_ = loaded.Writable();
    snippetsLoadMessage_ = loaded.message;
    if (!loaded.Writable()) {
      ReportPersistenceFailure(loaded.message.empty()
                                    ? L"Could not reload snippets.json."
                                    : loaded.message);
      return;
    }
    {
      std::lock_guard lock(dataMutex_);
      snippets_ = std::move(loaded.snippets);
    }
    MarkSearchDataChanged();
    RequestSearch();
    SetSettingsStatus(StatusSeverity::Success, L"Snippets reloaded.");
  }

  feathercast::library::OperationResult SaveManagedSnippets(
      const std::vector<feathercast::snippets::Snippet>& candidate) {
    if (!snippetsWritable_) {
      return {false, snippetsLoadMessage_.empty()
                         ? L"Snippet editing is unavailable."
                         : snippetsLoadMessage_};
    }
    for (const auto& item : candidate) if (AutomationKeywordExists(item.keyword)) return {false, L"That keyword is already used by a script or workspace."};
    {
      std::lock_guard lock(dataMutex_);
      snippets_ = candidate;
    }
    if (!QueueSnippetSave(candidate)) {
      return {false, L"The snippets worker is unavailable."};
    }
    MarkSearchDataChanged();
    RequestSearch();
    SetSettingsStatus(StatusSeverity::Progress, L"Saving snippets...");
    return {true, L"Snippet changes queued for saving."};
  }

  bool QueueManagedSettingsSave(Settings next) {
    if (!persistence_.SaveSettings(next)) return false;
    settings_ = std::move(next);
    SetSettingsStatus(StatusSeverity::Progress, L"Saving settings...");
    return true;
  }

  bool AutomationKeywordExists(const std::wstring& keyword) const {
    const auto normalized = feathercast::library::NormalizeKeyword(keyword);
    for (const auto* items : {&settings_.scripts, &settings_.workspaces}) {
      if (std::any_of(items->begin(), items->end(), [&](const auto& item) {
            return feathercast::library::NormalizeKeyword(item.keyword) == normalized;
          })) return true;
    }
    return false;
  }

  feathercast::library::OperationResult SaveManagedQuicklinks(
      const std::vector<Quicklink>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false,
              L"settings.json is protected from automatic writes. Quicklink editing is disabled."};
    }
    Settings next = settings_;
    for (const auto& item : candidate) if (AutomationKeywordExists(item.keyword)) return {false, L"That keyword is already used by a script or workspace."};
    next.quicklinks = candidate;
    if (!QueueManagedSettingsSave(std::move(next))) {
      return {false, L"The persistence worker is unavailable."};
    }
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"Quicklink changes queued for saving."};
  }

  feathercast::library::OperationResult SaveManagedAutomation(
      const std::vector<Quicklink>& candidate, feathercast::automation::Kind kind) {
    if (settingsPersistenceBlocked_) return {false, L"settings.json is protected from automatic writes."};
    if (candidate.size() > 256) return {false, L"The library supports up to 256 items of each type."};
    std::set<std::wstring> keywords;
    for (const auto& item : candidate) {
      if (const auto error = feathercast::automation::Validate(kind, item)) return {false, *error};
      if (!keywords.insert(feathercast::core::Normalize(item.keyword)).second) return {false, L"That keyword is already used."};
    }
    const auto& other = kind == feathercast::automation::Kind::Script ? settings_.workspaces : settings_.scripts;
    for (const auto* items : std::array<const std::vector<Quicklink>*, 2>{&settings_.quicklinks, &other}) {
      for (const auto& item : *items) if (keywords.contains(feathercast::core::Normalize(item.keyword))) return {false, L"That keyword is already used by another library item."};
    }
    for (const auto& [unused, alias] : settings_.commandAliases) if (keywords.contains(feathercast::core::Normalize(alias))) return {false, L"That keyword is already used by a command alias."};
    for (const auto& [unused, alias] : settings_.appAliases) if (keywords.contains(feathercast::core::Normalize(alias))) return {false, L"That keyword is already used by an app alias."};
    {
      std::lock_guard lock(dataMutex_);
      for (const auto& item : snippets_) if (keywords.contains(feathercast::library::NormalizeKeyword(item.keyword))) return {false, L"That keyword is already used by a snippet."};
    }
    Settings next = settings_;
    (kind == feathercast::automation::Kind::Script ? next.scripts : next.workspaces) = candidate;
    if (!QueueManagedSettingsSave(std::move(next))) return {false, L"The persistence worker is unavailable."};
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"Library changes queued for saving."};
  }

  feathercast::library::OperationResult SaveManagedCommandShortcuts(
      const std::vector<feathercast::library::CommandAlias>& candidate) {
    if (settingsPersistenceBlocked_) return {false, L"settings.json is protected from automatic writes."};
    Settings next = settings_;
    next.commandShortcuts.clear();
    std::set<std::wstring> chords;
    for (const auto& text : {settings_.shortcut, settings_.screenshotFullscreenShortcut,
                            settings_.screenshotRegionShortcut, settings_.recordFullscreenShortcut,
                            settings_.recordRegionShortcut}) {
      chords.insert(ParseShortcut(text).display);
    }
    for (const auto& item : candidate) {
      if (!feathercast::commands::Find(item.stableId)) return {false, L"Choose an available command."};
      if (const auto error = feathercast::automation::ValidateShortcut(item.alias)) return {false, *error};
      const auto chord = ParseShortcut(item.alias).display;
      if (!chords.insert(chord).second) return {false, L"That shortcut is already assigned to a FeatherCast action."};
      if (!next.commandShortcuts.emplace(item.stableId, chord).second) return {false, L"That command already has a shortcut. Edit the existing assignment."};
    }
    if (const auto error = commandHotKeys_.Assign(hwnd_, next.commandShortcuts)) return {false, *error};
    if (!QueueManagedSettingsSave(std::move(next))) {
      commandHotKeys_.Assign(hwnd_, settings_.commandShortcuts);
      return {false, L"The persistence worker is unavailable."};
    }
    return {true, L"Command shortcuts are active; changes queued for saving."};
  }

  feathercast::library::OperationResult SaveManagedAppAliases(
      const std::vector<feathercast::library::AppAlias>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false, L"settings.json is protected from automatic writes."};
    }
    Settings next = settings_;
    next.appAliases.clear();
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      const auto& entry = candidate[index];
      if (AutomationKeywordExists(entry.alias)) return {false, L"That alias is already used by a script or workspace."};
      if (const auto error = feathercast::library::ValidateAppAlias(
              entry, candidate, index)) {
        return {false, *error};
      }
      next.appAliases[entry.appId] = entry.alias;
    }
    if (!QueueManagedSettingsSave(std::move(next))) {
      return {false, L"The persistence worker is unavailable."};
    }
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"App-alias changes queued for saving."};
  }

  feathercast::library::OperationResult SaveManagedCommandAliases(
      const std::vector<feathercast::library::CommandAlias>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false, L"settings.json is protected from automatic writes."};
    }
    std::vector<feathercast::snippets::Snippet> currentSnippets;
    {
      std::lock_guard lock(dataMutex_);
      currentSnippets = snippets_;
    }
    std::vector<feathercast::library::AppAlias> appAliases;
    for (const auto& [appId, alias] : settings_.appAliases) {
      appAliases.push_back({appId, L"", alias});
    }
    std::wstring error;
    auto launchItems = settings_.quicklinks;
    launchItems.insert(launchItems.end(), settings_.scripts.begin(), settings_.scripts.end());
    launchItems.insert(launchItems.end(), settings_.workspaces.begin(), settings_.workspaces.end());
    const auto aliasMap = feathercast::library::ToCommandAliasMap(
        candidate, appAliases, currentSnippets, launchItems, &error);
    if (!aliasMap) {
      return {false, error.empty() ? L"Invalid command alias." : error};
    }
    Settings next = settings_;
    next.commandAliases = *aliasMap;
    if (!QueueManagedSettingsSave(std::move(next))) {
      return {false, L"The persistence worker is unavailable."};
    }
    MarkSearchDataChanged();
    RequestSearch();
    return {true, L"Command-alias changes queued for saving."};
  }

  feathercast::library::OperationResult SaveManagedWebSearches(
      const std::vector<feathercast::library::WebSearch>& candidate) {
    if (settingsPersistenceBlocked_) {
      return {false, L"settings.json is protected from automatic writes."};
    }
    Settings next = settings_;
    next.searchEngines.clear();
    for (std::size_t index = 0; index < candidate.size(); ++index) {
      if (const auto error = feathercast::library::ValidateWebSearch(
              candidate[index], candidate, index)) {
        return {false, *error};
      }
      next.searchEngines[feathercast::library::NormalizeKeyword(
          candidate[index].keyword)] = candidate[index].urlTemplate;
    }
    if (!QueueManagedSettingsSave(std::move(next))) {
      return {false, L"The persistence worker is unavailable."};
    }
    RequestSearch();
    return {true, L"Web-search changes queued for saving."};
  }

  feathercast::library::OperationResult RestoreDefaultWebSearches() {
    std::vector<feathercast::library::WebSearch> defaults;
    for (const auto& [keyword, urlTemplate] :
         feathercast::settings::DefaultSearchEngines()) {
      defaults.push_back({keyword, urlTemplate});
    }
    return SaveManagedWebSearches(defaults);
  }

  void RequestSnippetReload() {
    if (!QueueSnippetReload()) {
      ReportPersistenceFailure(L"The snippets worker is unavailable.");
    } else {
      SetSettingsStatus(StatusSeverity::Progress, L"Reloading snippets...");
    }
  }

  // The Library Manager is modal and shows the returned data right away, so
  // it reloads synchronously. A queued save holds newer data than the file,
  // so the in-memory snippets are returned while one is pending.
  feathercast::library_ui::ManagerData ReloadLibraryManagerData() {
    if (!SnippetSaveActive()) {
      feathercast::snippets_io::LoadResult loaded;
      {
        std::lock_guard lock(snippetsIoMutex_);
        loaded = feathercast::snippets_io::Load(SnippetsPath());
      }
      ApplySnippetReload(std::move(loaded));
    }
    return LibraryManagerData();
  }

  void OpenSnippetsFileForLibrary() {
    EnsureSnippetsFile();
    ShellExecuteW(nullptr, L"open", SnippetsPath().c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }

  void OpenLibraryManager(feathercast::library::ItemKind initialKind,
                          std::wstring initialAppId = {}) {
    if (libraryManagerOpen_) return;
    libraryManagerOpen_ = true;
    ScopeExit resetModal([this] { libraryManagerOpen_ = false; });
    feathercast::library_ui::ManagerCallbacks callbacks;
    callbacks.saveSnippets = [this](const auto& snippets) {
      return SaveManagedSnippets(snippets);
    };
    callbacks.saveQuicklinks = [this](const auto& quicklinks) {
      return SaveManagedQuicklinks(quicklinks);
    };
    callbacks.saveScripts = [this](const auto& items) {
      return SaveManagedAutomation(items, feathercast::automation::Kind::Script);
    };
    callbacks.saveWorkspaces = [this](const auto& items) {
      return SaveManagedAutomation(items, feathercast::automation::Kind::Workspace);
    };
    callbacks.saveCommandShortcuts = [this](const auto& items) {
      return SaveManagedCommandShortcuts(items);
    };
    callbacks.saveAppAliases = [this](const auto& aliases) {
      return SaveManagedAppAliases(aliases);
    };
    callbacks.saveCommandAliases = [this](const auto& aliases) {
      return SaveManagedCommandAliases(aliases);
    };
    callbacks.saveWebSearches = [this](const auto& searches) {
      return SaveManagedWebSearches(searches);
    };
    callbacks.restoreDefaultWebSearches = [this] {
      return RestoreDefaultWebSearches();
    };
    callbacks.reload = [this] { return ReloadLibraryManagerData(); };
    callbacks.openSnippetsFile = [this] { OpenSnippetsFileForLibrary(); };
    HWND owner = settingsHwnd_ && IsWindowVisible(settingsHwnd_)
                     ? settingsHwnd_
                     : hwnd_;
    feathercast::library_ui::ShowLibraryManager(
        owner, LibraryManagerData(), std::move(callbacks), initialKind,
        std::move(initialAppId), theme_, highContrast_);
    RepaintWindow(settingsHwnd_);
  }

