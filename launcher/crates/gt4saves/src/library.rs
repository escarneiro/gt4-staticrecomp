//! The save library (TASK-025; Documents layout TASK-081; one folder per save TASK-074, owner 2026-11-26):
//!
//! ```text
//! <saves>/<save name>/<game save>  one save per folder, named like the save (an empty folder is a save the game
//!                                  creates at its next Play)
//! <saves>/selected.txt             the name of the save the next Play uses
//! <saves>/.backup/                 copies of the selected save before a harvest replaces it (the newest 10)
//! <replays>/<replay>               every replay the library keeps
//! <backups>/                       card images written before they are replaced (the newest 10, and the newest 5
//!                                  taken before a New save, a delete or a switch of save)
//! ```
//!
//! Works like the memory cards of an emulator: every folder of `<saves>` is a save, the player picks one, and Play uses
//! it. Choosing another save moves nothing: only `selected.txt` changes. Renaming a save renames its folder. Folders
//! whose names start with a dot belong to the launcher and are not saves. There is always a selected save: when the
//! saves folder holds none, an empty "Default save" is made (see [`Library::open_named`]).
//!
//! Libraries written before this layout (`Active` for the active save, `Save NN` for the others, each with an optional
//! `name.txt` display name) are converted once when the library opens (see [`Library::convert_legacy_layout`]).
//!
//! The launcher puts `<saves>` and `<replays>` in the player's folder (see `gt4l_core::paths::UserDirs`) and `<backups>`
//! in the data folder. [`Library::open`] keeps all three under one root (tests and tools).
//!
//! Rules: an import never overwrites (it adds a save or skips an identical one); a harvest never deletes a save or a
//! replay (only backup copies beyond the limits of [`Library::prune_backups`]); the card built for Play holds exactly one
//! game save and the chosen replays. Every item is written to a staging folder and renamed into place
//! (`folder::export_item`).

use crate::card::{self, Card, CardError, Node};
use crate::names::SaveNames;
use crate::folder::{self, FolderError};
use crate::psu;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub enum LibraryError {
    Io(io::Error),
    Card(CardError),
    Folder(FolderError),
    /// The named item does not exist in the library.
    NotFound(String),
    /// The item is not something this library stores (for example another game's save).
    NotGtSave(String),
    /// A card with no Gran Turismo 4 save or replay on it; `found` lists the names it holds.
    NoGtSave { found: Vec<String> },
    /// A save name that cannot be used (empty after cleaning).
    Name(String),
}

impl From<io::Error> for LibraryError {
    fn from(error: io::Error) -> Self {
        LibraryError::Io(error)
    }
}

impl From<CardError> for LibraryError {
    fn from(error: CardError) -> Self {
        LibraryError::Card(error)
    }
}

impl From<FolderError> for LibraryError {
    fn from(error: FolderError) -> Self {
        LibraryError::Folder(error)
    }
}

impl fmt::Display for LibraryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LibraryError::Io(error) => write!(f, "{error}"),
            LibraryError::Card(error) => write!(f, "{error}"),
            LibraryError::Folder(error) => write!(f, "{error}"),
            LibraryError::NotFound(name) => write!(f, "not in the library: {name}"),
            LibraryError::NotGtSave(name) => write!(f, "not a Gran Turismo 4 save: {name}"),
            LibraryError::NoGtSave { found } => {
                write!(f, "no Gran Turismo 4 save on this card (found: {})", found.join(", "))
            }
            LibraryError::Name(reason) => write!(f, "{reason}"),
        }
    }
}

impl std::error::Error for LibraryError {}

/// What an import or harvest did, for the Saves and Replays pages.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct Report {
    /// Saves created (by save name) or replays added (by replay name).
    pub added: Vec<String>,
    /// Items already present with the same contents (nothing written).
    pub unchanged: Vec<String>,
    /// Items refused, with the reason.
    pub skipped: Vec<String>,
    /// The selected save was replaced; the old copy went to `<saves>/.backup`.
    pub replaced_active: bool,
}

/// The file in the saves folder that names the save the next Play uses.
pub const SELECTED_FILE: &str = "selected.txt";
/// The name of the save made when the saves folder holds none (the launcher passes its own, translated name).
pub const DEFAULT_SAVE_NAME: &str = "Default save";
/// The name an imported save gets when the caller gives none.
pub const IMPORTED_SAVE_NAME: &str = "Imported save";
/// Folder of the copies of the selected save made before a harvest replaces it, inside the saves folder.
pub const ACTIVE_BACKUP_FOLDER: &str = ".backup";
/// Longest save name, in characters.
pub const NAME_MAX_CHARS: usize = 40;

/// Before TASK-074: the folder of the active save inside the saves folder (still written by the one-time moves of older
/// libraries, `migration` and `gt4l_core::relocate`; [`Library::convert_legacy_layout`] turns it into a named save).
pub const ACTIVE_FOLDER: &str = "Active";
/// Before TASK-074: the prefix of an inactive slot folder (`Save 01`, `Save 02`, ...).
pub const SLOT_PREFIX: &str = "Save ";
/// Before TASK-074: the folder that held the active save during a swap.
pub const SWAP_FOLDER: &str = ".activate-swap";
/// Before TASK-074: the display name of a save, kept next to its game folder.
pub const NAME_FILE: &str = "name.txt";
/// Written in the saves folder once the old layout is converted, so a save the player names "Active" stays a save.
pub const LAYOUT_MARKER: &str = ".layout-folders";

/// `Save 01`, `Save 02`, ... (the folder names of the layout before TASK-074).
pub fn slot_name(number: u32) -> String {
    format!("Save {number:02}")
}

/// Names Windows refuses for a file or folder, whatever the extension.
const RESERVED_NAMES: &[&str] = &[
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
    "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
];

/// A save name made safe to use as a folder name on Windows and Linux: control characters and the characters Windows
/// refuses in file names are dropped, runs of spaces become one space, the ends are trimmed of spaces and dots (a leading
/// dot would hide the folder, a trailing one Windows drops), the text is cut to [`NAME_MAX_CHARS`], and a name Windows
/// reserves (`CON`, `COM1`, ...) gets " save" added. An empty result is an error.
pub fn clean_name(text: &str) -> Result<String, LibraryError> {
    let kept: String = text
        .chars()
        .map(|c| if c.is_control() { ' ' } else { c })
        .filter(|c| !matches!(c, '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*'))
        .collect();
    let spaced = kept.split_whitespace().collect::<Vec<_>>().join(" ");
    let trim = |text: &str| text.trim_matches(|c: char| c == '.' || c == ' ').to_string();
    let cut: String = trim(&spaced).chars().take(NAME_MAX_CHARS).collect();
    let mut cut = trim(&cut);
    if cut.is_empty() {
        return Err(LibraryError::Name("the name is empty".to_string()));
    }
    let stem = cut.split('.').next().unwrap_or("").trim().to_ascii_uppercase();
    if RESERVED_NAMES.contains(&stem.as_str()) {
        let room = NAME_MAX_CHARS - " save".len();
        cut = format!("{} save", trim(&cut.chars().take(room).collect::<String>()));
    }
    Ok(cut)
}

/// Where the three parts of the library live.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LibraryPaths {
    /// Game saves: one folder per save, `selected.txt` and `.backup`.
    pub saves: PathBuf,
    /// Replays, one folder each.
    pub replays: PathBuf,
    /// Card images kept before the runtime card is replaced (not player-facing).
    pub backups: PathBuf,
}

impl LibraryPaths {
    /// All three under one folder: `<root>/Saves`, `<root>/Replays`, `<root>/backups`.
    pub fn under(root: &Path) -> LibraryPaths {
        LibraryPaths { saves: root.join("Saves"), replays: root.join("Replays"), backups: root.join("backups") }
    }

    /// The library of one game (TASK-101): `<root>/games/<game id>/{Saves,Replays,backups}`. Two games never share a
    /// folder, so a save of one can never be harvested into, or built onto the card of, the other (each game has its
    /// own item names, see [`SaveNames`]).
    pub fn for_game(root: &Path, game_id: &str) -> LibraryPaths {
        LibraryPaths::under(&root.join("games").join(game_id))
    }
}

pub struct Library {
    paths: LibraryPaths,
    names: SaveNames,
    /// The name of the save made when the saves folder holds none.
    default_name: String,
}

impl Library {
    /// Opens a library kept under one folder (see [`LibraryPaths::under`]) for the game `names` describes.
    pub fn open(root: &Path, names: SaveNames) -> Result<Library, LibraryError> {
        Library::open_paths(&LibraryPaths::under(root), names)
    }

    /// Opens (and creates, if needed) a library at `paths`; a missing save is called [`DEFAULT_SAVE_NAME`].
    pub fn open_paths(paths: &LibraryPaths, names: SaveNames) -> Result<Library, LibraryError> {
        Library::open_named(paths, names, DEFAULT_SAVE_NAME)
    }

    /// Opens (and creates, if needed) a library at `paths`. A library of the layout before TASK-074 is converted first.
    /// When the saves folder holds no save, an empty one named `default_name` is made; when no save is selected (or the
    /// selected one is gone) the first save in name order is.
    pub fn open_named(paths: &LibraryPaths, names: SaveNames, default_name: &str) -> Result<Library, LibraryError> {
        for folder in [paths.saves.join(ACTIVE_BACKUP_FOLDER), paths.replays.clone(), paths.backups.clone()] {
            fs::create_dir_all(folder)?;
        }
        let default_name = clean_name(default_name).unwrap_or_else(|_| DEFAULT_SAVE_NAME.to_string());
        let library = Library { paths: paths.clone(), names, default_name };
        library.convert_legacy_layout()?;
        library.ensure_selected()?;
        Ok(library)
    }

    pub fn paths(&self) -> &LibraryPaths {
        &self.paths
    }

    /// The item names of the game this library belongs to.
    pub fn names(&self) -> SaveNames {
        self.names
    }

    /// The name a save made because none exists gets.
    pub fn default_name(&self) -> &str {
        &self.default_name
    }

    /// Folder of the copies of the selected save made before a harvest replaced it.
    pub fn active_backup_dir(&self) -> PathBuf {
        self.paths.saves.join(ACTIVE_BACKUP_FOLDER)
    }

    /// Folder that holds every replay.
    pub fn replays_dir(&self) -> PathBuf {
        self.paths.replays.clone()
    }

    fn saves_dir(&self) -> PathBuf {
        self.paths.saves.clone()
    }

    /// The folder of the save `name` (it may not exist).
    pub fn save_dir(&self, name: &str) -> PathBuf {
        self.saves_dir().join(name)
    }

    /// The game data folder of the save `name` (`<saves>/<name>/<game save>`; it may not exist).
    pub fn game_data_dir(&self, name: &str) -> PathBuf {
        self.save_dir(name).join(self.names.game_save)
    }

    /// Whether the save `name` holds game data (an empty save is one the game creates at its next Play).
    pub fn has_game_data(&self, name: &str) -> bool {
        self.game_data_dir(name).is_dir()
    }

    /// Every save: the folders of the saves folder whose names do not start with a dot, in name order (letter case
    /// ignored).
    pub fn saves(&self) -> Result<Vec<String>, LibraryError> {
        let mut saves: Vec<String> = fs::read_dir(self.saves_dir())?
            .filter_map(|entry| entry.ok())
            .filter(|entry| entry.path().is_dir())
            .map(|entry| entry.file_name().to_string_lossy().into_owned())
            .filter(|name| !name.starts_with('.'))
            .collect();
        saves.sort_by(|a, b| a.to_lowercase().cmp(&b.to_lowercase()).then_with(|| a.cmp(b)));
        Ok(saves)
    }

    /// The save `name` as the saves folder spells it, matched without letter case (folders on Windows ignore it).
    fn find_save(&self, name: &str) -> Result<Option<String>, LibraryError> {
        let wanted = name.trim().to_lowercase();
        Ok(self.saves()?.into_iter().find(|save| save.to_lowercase() == wanted))
    }

    /// The save the next Play uses, when `selected.txt` names one that exists.
    pub fn selected(&self) -> Result<Option<String>, LibraryError> {
        let text = match fs::read_to_string(self.saves_dir().join(SELECTED_FILE)) {
            Ok(text) => text,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
            Err(error) => return Err(error.into()),
        };
        let name = text.trim();
        if name.is_empty() {
            return Ok(None);
        }
        self.find_save(name)
    }

    /// Writes `selected.txt` (through a temporary file, so it is never half written).
    fn write_selected(&self, name: &str) -> Result<(), LibraryError> {
        let file = self.saves_dir().join(SELECTED_FILE);
        let temporary = self.saves_dir().join(".selected.tmp");
        fs::write(&temporary, name.as_bytes())?;
        fs::rename(&temporary, &file)?;
        Ok(())
    }

    /// Makes `name` the save the next Play uses. Nothing is moved.
    pub fn select(&self, name: &str) -> Result<String, LibraryError> {
        let found = self.find_save(name)?.ok_or_else(|| LibraryError::NotFound(name.to_string()))?;
        self.write_selected(&found)?;
        Ok(found)
    }

    /// The selected save. When none is selected (or the selected one was removed) the first save in name order becomes
    /// it; when the saves folder holds no save, an empty save named [`Library::default_name`] is made and selected.
    pub fn ensure_selected(&self) -> Result<String, LibraryError> {
        if let Some(selected) = self.selected()? {
            return Ok(selected);
        }
        let chosen = match self.saves()?.into_iter().next() {
            Some(first) => first,
            None => self.create_save(&self.default_name.clone())?,
        };
        self.write_selected(&chosen)?;
        Ok(chosen)
    }

    /// The name of the selected save when it holds game data.
    pub fn active_save(&self) -> Result<Option<String>, LibraryError> {
        let selected = self.ensure_selected()?;
        Ok(self.has_game_data(&selected).then_some(selected))
    }

    /// The folder of the selected save.
    pub fn active_dir(&self) -> Result<PathBuf, LibraryError> {
        Ok(self.save_dir(&self.ensure_selected()?))
    }

    /// The game data folder of the selected save (it may not exist yet).
    pub fn active_save_dir(&self) -> Result<PathBuf, LibraryError> {
        Ok(self.active_dir()?.join(self.names.game_save))
    }

    /// `wanted` after [`clean_name`], or `wanted (2)`, `wanted (3)`, ... when another save already has that name (letter
    /// case ignored). `except` is a save that does not count (the one being renamed).
    pub fn unique_name(&self, wanted: &str, except: Option<&str>) -> Result<String, LibraryError> {
        let except = except.map(str::to_lowercase);
        let taken: Vec<String> = self
            .saves()?
            .into_iter()
            .map(|name| name.to_lowercase())
            .filter(|name| Some(name) != except.as_ref())
            .collect();
        let base = clean_name(wanted)?;
        let mut candidate = base.clone();
        let mut number = 2;
        while taken.contains(&candidate.to_lowercase()) {
            // The suffix always fits: the base is cut so that " (N)" stays inside NAME_MAX_CHARS.
            let suffix = format!(" ({number})");
            let room = NAME_MAX_CHARS.saturating_sub(suffix.chars().count());
            let cut: String = base.chars().take(room).collect();
            candidate = clean_name(&format!("{}{suffix}", cut.trim_end()))?;
            number += 1;
        }
        Ok(candidate)
    }

    /// Makes an empty save named `wanted` (made unique, see [`Library::unique_name`]). The selection does not change.
    /// Returns the name used.
    pub fn create_save(&self, wanted: &str) -> Result<String, LibraryError> {
        let name = self.unique_name(wanted, None)?;
        fs::create_dir_all(self.save_dir(&name))?;
        Ok(name)
    }

    /// Starts a new save: an empty save named `wanted` that becomes the selected one, so the next Play makes a new
    /// profile. The save that was selected stays as it is. Returns the name used.
    pub fn new_save(&self, wanted: &str) -> Result<String, LibraryError> {
        let name = self.create_save(wanted)?;
        self.write_selected(&name)?;
        Ok(name)
    }

    /// Renames the save `name` (its folder) to `new_name`, made unique. The selection follows the save. Returns the name
    /// used.
    pub fn rename_save(&self, name: &str, new_name: &str) -> Result<String, LibraryError> {
        let current = self.find_save(name)?.ok_or_else(|| LibraryError::NotFound(name.to_string()))?;
        let target = self.unique_name(new_name, Some(&current))?;
        if target == current {
            return Ok(target);
        }
        let was_selected = self.selected()?.as_deref() == Some(current.as_str());
        if target.to_lowercase() == current.to_lowercase() {
            // Only the letter case changes: a folder system that ignores case needs a step through another name.
            let step = self.saves_dir().join(format!(".rename-{}", crate::timestamp()));
            fs::rename(self.save_dir(&current), &step)?;
            fs::rename(&step, self.save_dir(&target))?;
        } else {
            fs::rename(self.save_dir(&current), self.save_dir(&target))?;
        }
        if was_selected {
            self.write_selected(&target)?;
        }
        Ok(target)
    }

    /// Every replay the library keeps, in name order.
    pub fn replays(&self) -> Result<Vec<String>, LibraryError> {
        let mut names: Vec<String> = fs::read_dir(self.replays_dir())?
            .filter_map(|e| e.ok())
            .filter(|e| e.path().is_dir())
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| self.names.is_replay(n))
            .collect();
        names.sort();
        Ok(names)
    }

    /// The save that already holds these exact game data, if any.
    fn save_holding(&self, node: &Node) -> Result<Option<String>, LibraryError> {
        for save in self.saves()? {
            let path = self.game_data_dir(&save);
            if !path.is_dir() {
                continue;
            }
            let existing = folder::import_item(&path)?;
            if card::same_contents(std::slice::from_ref(&existing), std::slice::from_ref(node)) {
                return Ok(Some(save));
            }
        }
        Ok(None)
    }

    /// First unused replay name `BASCUS-97328Rnnnn` in the replays folder (letter R, four digits). Used only when two
    /// different replays share a name; Spec II's own names are seven characters, and whether Spec II lists a replay named
    /// this way is not verified.
    fn next_free_replay_name(&self) -> Result<String, LibraryError> {
        for number in 1..=9999 {
            let name = self.names.full_replay_name(number);
            if !self.replays_dir().join(&name).exists() {
                return Ok(name);
            }
        }
        Err(LibraryError::Card(CardError::Full))
    }

    /// Adds `node` (a game save or replay) to the library; a game save becomes a new save named `save_name`.
    fn add_item(&self, node: &Node, save_name: &str, report: &mut Report) -> Result<(), LibraryError> {
        let name = node.name();
        if self.names.is_game_save(&name) {
            if let Some(holder) = self.save_holding(node)? {
                report.unchanged.push(holder);
                return Ok(());
            }
            // Every other game save becomes a new save; the selection never changes on import.
            let save = self.create_save(save_name)?;
            folder::export_item(&self.save_dir(&save), node)?;
            report.added.push(save);
        } else if self.names.is_replay(&name) {
            self.add_replay(node, report)?;
        } else {
            report.skipped.push(name);
        }
        Ok(())
    }

    fn add_replay(&self, node: &Node, report: &mut Report) -> Result<(), LibraryError> {
        let name = node.name();
        let existing = self.replays_dir().join(&name);
        if existing.is_dir() && card::same_contents(std::slice::from_ref(&folder::import_item(&existing)?), std::slice::from_ref(node)) {
            report.unchanged.push(name);
        } else if existing.is_dir() {
            // Same name, other contents: keep both, under a fresh name.
            let renamed = self.next_free_replay_name()?;
            let mut copy = node.clone();
            copy.entry.set_name(&renamed)?;
            folder::export_item(&self.replays_dir(), &copy)?;
            report.added.push(renamed);
        } else {
            folder::export_item(&self.replays_dir(), node)?;
            report.added.push(name);
        }
        Ok(())
    }

    /// Imports the items this library owns (its game save and replays) from `items`; a game save becomes a save named
    /// [`IMPORTED_SAVE_NAME`]. See [`Library::import_items_named`].
    pub fn import_items(&self, items: &[Node]) -> Result<Report, LibraryError> {
        self.import_items_named(items, IMPORTED_SAVE_NAME)
    }

    /// Imports the items this library owns (its game save and replays) from `items`. A game save becomes a new save named
    /// `save_name` (made unique); one the library already holds is reported unchanged, under the save that holds it.
    /// Items of other games, and items no game knows, are not stored: they come back in `Report::skipped` by name. When
    /// none of the items is this game's, the import fails with [`LibraryError::NoGtSave`].
    pub fn import_items_named(&self, items: &[Node], save_name: &str) -> Result<Report, LibraryError> {
        if !items.iter().any(|item| self.names.owns(&item.name())) {
            return Err(LibraryError::NoGtSave { found: items.iter().map(Node::name).collect() });
        }
        let save_name = clean_name(save_name).unwrap_or_else(|_| IMPORTED_SAVE_NAME.to_string());
        let mut report = Report::default();
        for item in items {
            self.add_item(item, &save_name, &mut report)?;
        }
        Ok(report)
    }

    /// Imports every item of a card image (`.ps2`, `.mcd`, `.bin`, with or without spare bytes).
    pub fn import_card_image(&self, bytes: &[u8]) -> Result<Report, LibraryError> {
        self.import_items(&items_of_card_image(bytes)?)
    }

    /// Imports a `.psu` save file.
    pub fn import_psu(&self, bytes: &[u8]) -> Result<Report, LibraryError> {
        self.import_items(&[psu::read(bytes)?])
    }

    /// Imports a PCSX2 folder memory card: each save sub-folder is a raw directory.
    pub fn import_folder_card(&self, folder_card: &Path) -> Result<Report, LibraryError> {
        self.import_items(&items_of_folder_card(folder_card)?)
    }

    /// `.psu` bytes of the save `name`.
    pub fn export_save_psu(&self, name: &str) -> Result<Vec<u8>, LibraryError> {
        let save = self.find_save(name)?.ok_or_else(|| LibraryError::NotFound(name.to_string()))?;
        let path = self.game_data_dir(&save);
        if !path.is_dir() {
            return Err(LibraryError::NotFound(name.to_string()));
        }
        Ok(psu::write(&folder::import_item(&path)?)?)
    }

    /// `.psu` bytes of the replay `name`.
    pub fn export_replay_psu(&self, name: &str) -> Result<Vec<u8>, LibraryError> {
        let path = self.replays_dir().join(name);
        if !self.names.is_replay(name) || !path.is_dir() {
            return Err(LibraryError::NotFound(name.to_string()));
        }
        Ok(psu::write(&folder::import_item(&path)?)?)
    }

    /// Builds the card image for the next Play: the selected save's game data (none for an empty save) plus `replays`.
    /// The image is verified by reading it back before it is returned.
    pub fn build_card(&self, replays: &[String]) -> Result<Vec<u8>, LibraryError> {
        let mut items = Vec::new();
        let game_data = self.active_save_dir()?;
        if game_data.is_dir() {
            items.push(folder::import_item(&game_data)?);
        }
        for name in replays {
            let path = self.replays_dir().join(name);
            if !path.is_dir() {
                return Err(LibraryError::NotFound(name.clone()));
            }
            items.push(folder::import_item(&path)?);
        }
        let card = Card::build(&items)?;
        let bytes = card.to_image();
        let readback = Card::from_image(&bytes)?.root()?;
        if !card::same_contents(&readback, &items) {
            return Err(LibraryError::Card(CardError::Corrupt("built card does not read back identical".into())));
        }
        Ok(bytes)
    }

    /// Copies the card image written by a Play back into the library, without deleting anything: changed game data
    /// replace those of the selected save (the old copy goes to `<saves>/.backup`); new replays are added.
    pub fn harvest(&self, card_bytes: &[u8]) -> Result<Report, LibraryError> {
        let card = Card::from_image(card_bytes)?;
        let mut report = Report::default();
        for item in card.root()? {
            let name = item.name();
            if self.names.is_game_save(&name) {
                let save_dir = self.active_dir()?;
                let current = save_dir.join(&name);
                if current.is_dir() {
                    let before = folder::import_item(&current)?;
                    if card::same_contents(std::slice::from_ref(&before), std::slice::from_ref(&item)) {
                        report.unchanged.push(name);
                        continue;
                    }
                    let backup = self.active_backup_dir();
                    let stamp = crate::timestamp();
                    let copy_dir = backup.join(format!("{name}-{stamp}"));
                    fs::create_dir_all(&copy_dir)?;
                    folder::export_item(&copy_dir, &before)?;
                    // The oldest copies beyond KEPT_ACTIVE_COPIES go; a copy that cannot be removed now is tried again later.
                    let _ = self.prune_backups();
                    report.replaced_active = true;
                }
                fs::create_dir_all(&save_dir)?;
                folder::export_item(&save_dir, &item)?;
                report.added.push(name);
            } else if self.names.is_replay(&name) {
                self.add_replay(&item, &mut report)?;
            } else {
                report.skipped.push(name);
            }
        }
        Ok(report)
    }

    /// Writes a card image to `path`: the previous file (if any) is copied to backups/ first,
    /// and the new image is written to a temporary file and renamed into place.
    pub fn write_card_image(&self, path: &Path, bytes: &[u8]) -> Result<(), LibraryError> {
        if path.is_file() {
            fs::create_dir_all(&self.paths.backups)?;
            let backup = self.paths.backups.join(format!("{}-{}", crate::timestamp(), file_name(path)));
            fs::copy(path, backup)?;
        }
        let temporary = path.with_extension("tmp");
        fs::write(&temporary, bytes)?;
        fs::rename(&temporary, path)?;
        // An old backup that cannot be removed (open in another program, say) must not stop the game from starting: it is
        // tried again at the next Play.
        let _ = self.prune_backups();
        Ok(())
    }

    /// Keeps the newest backups of each kind and deletes the older ones (owner, 2026-11-03: every Play added a card image of
    /// about 8 MB to `<backups>` and nothing removed them, 74 images in two weeks on the owner's PC). The kinds, by name:
    /// card images `<timestamp>-<card file>.ps2` ([`KEPT_CARD_BACKUPS`]); images taken before a New save, a delete of the
    /// selected save or a switch of save, `<timestamp>-before-new-save.ps2`, `<timestamp>-before-delete.ps2`,
    /// `<timestamp>-before-switch.ps2` ([`KEPT_NEW_SAVE_BACKUPS`]); and copies of the selected save
    /// `<saves>/.backup/<game save>-<timestamp>` ([`KEPT_ACTIVE_COPIES`]). A name of any other form is never touched.
    /// Returns how many were deleted.
    pub fn prune_backups(&self) -> Result<usize, LibraryError> {
        let mut deleted = 0;
        let backups = &self.paths.backups;
        if backups.is_dir() {
            deleted += keep_newest(backups, KEPT_CARD_BACKUPS, |name| is_card_backup(name), |path| fs::remove_file(path))?;
            deleted += keep_newest(backups, KEPT_NEW_SAVE_BACKUPS, |name| is_new_save_backup(name), |path| fs::remove_file(path))?;
        }
        let copies = self.active_backup_dir();
        if copies.is_dir() {
            let game_save = self.names.game_save;
            deleted += keep_newest(&copies, KEPT_ACTIVE_COPIES, |name| is_active_copy(name, game_save), |path| fs::remove_dir_all(path))?;
        }
        Ok(deleted)
    }

    /// TASK-074: turns a library of the layout before one folder per save into the new layout, once (the saves folder then
    /// holds [`LAYOUT_MARKER`]). A swap that was interrupted is finished first: the save waiting in `.activate-swap`
    /// goes back to `Active` when that is empty, else it becomes a save of its own. Then every `Save NN` slot with game
    /// data and `Active` (even empty: it is the save the next Play creates) become saves named by their `name.txt`
    /// (else the slot keeps its folder name and `Active` gets the default name); `Active` becomes the selected save.
    /// No game data move between folders except by renaming the folders that hold them.
    fn convert_legacy_layout(&self) -> Result<(), LibraryError> {
        let saves = self.saves_dir();
        let marker = saves.join(LAYOUT_MARKER);
        if marker.is_file() {
            return Ok(());
        }
        fs::create_dir_all(&saves)?;
        let active = saves.join(ACTIVE_FOLDER);
        let legacy_name = |folder: &Path| -> Option<String> {
            let text = fs::read_to_string(folder.join(NAME_FILE)).ok()?;
            clean_name(text.trim()).ok()
        };
        // An interrupted swap of the old activate_slot.
        let swap = saves.join(SWAP_FOLDER);
        if swap.is_dir() {
            if swap.join(self.names.game_save).is_dir() {
                let target = if active.join(self.names.game_save).is_dir() {
                    let wanted = legacy_name(&swap).unwrap_or_else(|| self.default_name.clone());
                    self.save_dir(&self.unique_name(&wanted, None)?)
                } else {
                    active.clone()
                };
                fs::create_dir_all(&target)?;
                fs::rename(swap.join(self.names.game_save), target.join(self.names.game_save))?;
                if target != active {
                    // The name went with the save; a stale name of the empty Active must not stick to it.
                    let _ = fs::remove_file(swap.join(NAME_FILE));
                } else if swap.join(NAME_FILE).is_file() {
                    let _ = fs::remove_file(active.join(NAME_FILE));
                    fs::rename(swap.join(NAME_FILE), active.join(NAME_FILE))?;
                }
            }
            let _ = fs::remove_file(swap.join(NAME_FILE));
            fs::remove_dir(&swap)?;
        }
        // The slots, which mostly keep their folder names; then Active, which always gets a new one.
        let slots: Vec<String> = self
            .saves()?
            .into_iter()
            .filter(|name| name.strip_prefix(SLOT_PREFIX).is_some_and(|n| !n.is_empty() && n.chars().all(|c| c.is_ascii_digit())))
            .filter(|name| self.has_game_data(name) || self.save_dir(name).join(NAME_FILE).is_file())
            .collect();
        for slot in slots {
            let folder = self.save_dir(&slot);
            if let Some(wanted) = legacy_name(&folder) {
                let target = self.unique_name(&wanted, Some(&slot))?;
                if target != slot {
                    fs::rename(&folder, self.save_dir(&target))?;
                }
                let _ = fs::remove_file(self.save_dir(&target).join(NAME_FILE));
            }
        }
        if active.is_dir() {
            let wanted = legacy_name(&active).unwrap_or_else(|| self.default_name.clone());
            let target = self.unique_name(&wanted, Some(ACTIVE_FOLDER))?;
            let _ = fs::remove_file(active.join(NAME_FILE));
            if target != ACTIVE_FOLDER {
                fs::rename(&active, self.save_dir(&target))?;
            }
            self.write_selected(&target)?;
        }
        fs::write(&marker, b"one folder per save (TASK-074)\n")?;
        Ok(())
    }
}

/// Card images kept in `<backups>` (about 8 MB each).
pub const KEPT_CARD_BACKUPS: usize = 10;
/// Card images taken before a New save, kept in `<backups>`.
pub const KEPT_NEW_SAVE_BACKUPS: usize = 5;
/// Copies of the active save kept in `<saves>/.backup`.
pub const KEPT_ACTIVE_COPIES: usize = 10;
/// The end of the name of a card image taken before a New save (the launcher's `new_save`).
pub const NEW_SAVE_BACKUP_SUFFIX: &str = "-before-new-save.ps2";
/// The end of the name of a card image taken before the selected save is deleted (kept with the New save images).
pub const DELETE_BACKUP_SUFFIX: &str = "-before-delete.ps2";
/// The end of the name of a card image taken before another save is selected (kept with the New save images).
pub const SWITCH_BACKUP_SUFFIX: &str = "-before-switch.ps2";

/// True for `YYYYMMDD-HHMMSS`, the form of [`crate::timestamp`].
fn is_timestamp(text: &str) -> bool {
    let bytes = text.as_bytes();
    bytes.len() == 15 && bytes.iter().enumerate().all(|(index, byte)| if index == 8 { *byte == b'-' } else { byte.is_ascii_digit() })
}

/// `<timestamp>-<card file>.ps2`, not taken before a New save.
fn is_card_backup(name: &str) -> bool {
    name.get(..15).is_some_and(is_timestamp)
        && name.get(15..16) == Some("-")
        && name.len() > 20
        && name.ends_with(".ps2")
        && !name.ends_with(NEW_SAVE_BACKUP_SUFFIX)
        && !name.ends_with(DELETE_BACKUP_SUFFIX)
        && !name.ends_with(SWITCH_BACKUP_SUFFIX)
}

/// `<timestamp>-before-new-save.ps2`, `<timestamp>-before-delete.ps2` or `<timestamp>-before-switch.ps2`.
fn is_new_save_backup(name: &str) -> bool {
    name.strip_suffix(NEW_SAVE_BACKUP_SUFFIX)
        .or_else(|| name.strip_suffix(DELETE_BACKUP_SUFFIX))
        .or_else(|| name.strip_suffix(SWITCH_BACKUP_SUFFIX))
        .is_some_and(is_timestamp)
}

/// `<game save>-<timestamp>`.
fn is_active_copy(name: &str, game_save: &str) -> bool {
    name.strip_prefix(game_save).and_then(|rest| rest.strip_prefix('-')).is_some_and(is_timestamp)
}

/// Deletes all but the `keep` newest entries of `dir` whose names `kind` accepts, and returns how many it deleted. Every kind
/// has a fixed part and a timestamp in the same place, so name order is age order.
fn keep_newest(
    dir: &Path,
    keep: usize,
    kind: impl Fn(&str) -> bool,
    remove: impl Fn(&Path) -> io::Result<()>,
) -> Result<usize, LibraryError> {
    let mut names: Vec<String> = fs::read_dir(dir)?
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.file_name().to_string_lossy().into_owned())
        .filter(|name| kind(name))
        .collect();
    if names.len() <= keep {
        return Ok(0);
    }
    names.sort();
    let excess = names.len() - keep;
    for name in &names[..excess] {
        remove(&dir.join(name))?;
    }
    Ok(excess)
}

/// The items of a card image (`.ps2`, `.mcd`, `.bin`, with or without spare bytes), whichever game they belong to.
pub fn items_of_card_image(bytes: &[u8]) -> Result<Vec<Node>, LibraryError> {
    Ok(Card::from_image(bytes)?.root()?)
}

/// The item of a `.psu` save file.
pub fn items_of_psu(bytes: &[u8]) -> Result<Vec<Node>, LibraryError> {
    Ok(vec![psu::read(bytes)?])
}

/// The items of a PCSX2 folder memory card: each save sub-folder is a raw directory.
pub fn items_of_folder_card(folder_card: &Path) -> Result<Vec<Node>, LibraryError> {
    let mut dirs: Vec<PathBuf> = fs::read_dir(folder_card)?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    dirs.sort();
    dirs.iter().map(|dir| Ok(folder::import_raw_directory(dir)?)).collect()
}

fn file_name(path: &Path) -> String {
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "card".into())
}
