// Library rules for the Saves page (TASK-104; one folder per save TASK-074): the saves folder lists the saves, one is
// selected, New save, Export, Rename, the default save and the conversion of the old Active / Save NN layout.

mod common;

use gt4saves::SaveNames;
#[allow(dead_code)]
const GT4: SaveNames = SaveNames::new("BASCUS-97328GAMEDATA", "BASCUS-97328");
#[allow(dead_code)]
const SPEC2: SaveNames = SaveNames::new("BASCUS-97436GAMEDATA", "BASCUS-97436");
#[allow(dead_code)]
const GT4_GAME_SAVE: &str = "BASCUS-97328GAMEDATA";
use common::*;
use gt4saves::card::{same_contents, Node};
use gt4saves::library::{
    clean_name, LibraryError, LibraryPaths, ACTIVE_FOLDER, DEFAULT_SAVE_NAME, IMPORTED_SAVE_NAME, LAYOUT_MARKER, NAME_FILE,
    NAME_MAX_CHARS, SELECTED_FILE, SWAP_FOLDER,
};
use gt4saves::{folder, psu, Card, Library};
use std::fs;
use std::path::{Path, PathBuf};

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("manage-{name}"));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn card_of(items: &[Node]) -> Vec<u8> {
    Card::build(items).unwrap().to_image()
}

fn selected_file(library: &Library) -> String {
    fs::read_to_string(library.paths().saves.join(SELECTED_FILE)).unwrap()
}

/// The game data a card built now carries (`None` for a card without a game save).
fn built_save(library: &Library) -> Option<Node> {
    let card = Card::from_image(&library.build_card(&[]).unwrap()).unwrap();
    card.root().unwrap().into_iter().find(|item| item.name() == GT4_GAME_SAVE)
}

fn same(a: &Node, b: &Node) -> bool {
    same_contents(std::slice::from_ref(a), std::slice::from_ref(b))
}

#[test]
fn a_new_library_has_one_empty_default_save_and_it_is_selected() {
    let library = Library::open(&scratch("default"), GT4).unwrap();
    assert_eq!(library.saves().unwrap(), [DEFAULT_SAVE_NAME]);
    assert_eq!(library.selected().unwrap().as_deref(), Some(DEFAULT_SAVE_NAME));
    assert_eq!(selected_file(&library), DEFAULT_SAVE_NAME);
    assert_eq!(library.active_save().unwrap(), None, "the default save is empty: the game creates it at the next Play");
    assert!(built_save(&library).is_none());
    // The launcher's own folders and files are not saves.
    assert!(library.active_backup_dir().is_dir());
    assert!(library.paths().saves.join(LAYOUT_MARKER).is_file());
}

#[test]
fn the_default_save_takes_the_name_the_launcher_gives() {
    let root = scratch("default-named");
    let library = Library::open_named(&LibraryPaths::under(&root), GT4, "Save padrão").unwrap();
    assert_eq!(library.saves().unwrap(), ["Save padrão"]);
}

#[test]
fn every_folder_of_the_saves_folder_is_a_save_and_the_selection_moves_nothing() {
    let library = Library::open(&scratch("folders"), GT4).unwrap();
    library.import_items_named(&[game_save(1)], "Career").unwrap();
    library.import_items_named(&[game_save(2)], "arcade").unwrap();
    // A folder the player made by hand, and a folder of the launcher (dot), and a loose file.
    fs::create_dir_all(library.save_dir("Copied by hand")).unwrap();
    folder::export_item(&library.save_dir("Copied by hand"), &game_save(3)).unwrap();
    fs::create_dir_all(library.paths().saves.join(".hidden")).unwrap();
    fs::write(library.paths().saves.join("card.ps2"), b"loose").unwrap();
    assert_eq!(library.saves().unwrap(), ["arcade", "Career", "Copied by hand", DEFAULT_SAVE_NAME], "name order, case ignored");

    for (name, seed) in [("Career", 1), ("arcade", 2), ("Copied by hand", 3)] {
        let before = library.saves().unwrap();
        assert_eq!(library.select(name).unwrap(), name);
        assert_eq!(library.saves().unwrap(), before, "no folder moves or changes its name");
        assert_eq!(library.active_save().unwrap().as_deref(), Some(name));
        assert!(same(&built_save(&library).unwrap(), &game_save(seed)), "Play uses {name}");
    }
    // The selection matches without letter case, and is written the way the folder is spelled.
    assert_eq!(library.select("CAREER").unwrap(), "Career");
    assert_eq!(selected_file(&library), "Career");
    assert!(matches!(library.select("Nope"), Err(LibraryError::NotFound(_))));
    assert_eq!(library.selected().unwrap().as_deref(), Some("Career"), "a refused selection changes nothing");
}

#[test]
fn a_harvest_goes_into_the_selected_save_only() {
    let library = Library::open(&scratch("harvest-selected"), GT4).unwrap();
    library.import_items_named(&[game_save(1)], "One").unwrap();
    library.import_items_named(&[game_save(2)], "Two").unwrap();
    library.select("Two").unwrap();
    let report = library.harvest(&card_of(&[game_save(9)])).unwrap();
    assert!(report.replaced_active);
    assert!(same(&folder::import_item(&library.game_data_dir("Two")).unwrap(), &game_save(9)));
    assert!(same(&folder::import_item(&library.game_data_dir("One")).unwrap(), &game_save(1)), "the other save is untouched");
    assert!(!library.has_game_data(DEFAULT_SAVE_NAME));
}

#[test]
fn an_empty_selected_save_is_filled_by_the_first_harvest() {
    let library = Library::open(&scratch("harvest-empty"), GT4).unwrap();
    let report = library.harvest(&card_of(&[game_save(7)])).unwrap();
    assert_eq!(report.added, [GT4_GAME_SAVE]);
    assert!(!report.replaced_active, "nothing was replaced: the save was empty");
    assert_eq!(library.active_save().unwrap().as_deref(), Some(DEFAULT_SAVE_NAME));
    assert_eq!(library.saves().unwrap(), [DEFAULT_SAVE_NAME]);
    // A harvest of a card with no game save changes nothing.
    let report = library.harvest(&card_of(&[])).unwrap();
    assert!(report.added.is_empty() && !report.replaced_active);
}

#[test]
fn new_save_makes_an_empty_selected_save_and_keeps_the_old_one() {
    let library = Library::open(&scratch("new-save"), GT4).unwrap();
    library.harvest(&card_of(&[game_save(1)])).unwrap();
    assert_eq!(library.new_save("New save").unwrap(), "New save");
    assert_eq!(library.selected().unwrap().as_deref(), Some("New save"));
    assert_eq!(library.active_save().unwrap(), None);
    assert!(built_save(&library).is_none(), "the next Play starts a new profile");
    assert!(library.has_game_data(DEFAULT_SAVE_NAME), "the old save stays where it was");
    // A second new save gets a free name.
    assert_eq!(library.new_save("new SAVE").unwrap(), "new SAVE (2)");
    assert_eq!(library.saves().unwrap(), [DEFAULT_SAVE_NAME, "New save", "new SAVE (2)"]);
}

#[test]
fn an_import_is_named_after_the_caller_and_names_stay_unique() {
    let library = Library::open(&scratch("import-names"), GT4).unwrap();
    assert_eq!(library.import_items_named(&[game_save(1)], "gran-turismo-4.22070").unwrap().added, ["gran-turismo-4.22070"]);
    assert_eq!(library.import_items_named(&[game_save(2)], "gran-turismo-4.22070").unwrap().added, ["gran-turismo-4.22070 (2)"]);
    assert_eq!(library.import_items(&[game_save(3)]).unwrap().added, [IMPORTED_SAVE_NAME]);
    // An import never changes the selection.
    assert_eq!(library.selected().unwrap().as_deref(), Some(DEFAULT_SAVE_NAME));
    // The same data again is found in the save that holds them.
    let again = library.import_items_named(&[game_save(2)], "x").unwrap();
    assert!(again.added.is_empty());
    assert_eq!(again.unchanged, ["gran-turismo-4.22070 (2)"]);
}

#[test]
fn an_import_of_a_card_with_no_gt4_item_names_what_it_found() {
    let library = Library::open(&scratch("no-gt4"), GT4).unwrap();
    let other = dir("SLUS-20000GAMEDATA", vec![file("icon.sys", pattern(964, 9))]);
    match library.import_card_image(&card_of(&[other])) {
        Err(LibraryError::NoGtSave { found }) => assert_eq!(found, ["SLUS-20000GAMEDATA"]),
        other => panic!("expected NoGtSave, got {other:?}"),
    }
    match library.import_card_image(&card_of(&[])) {
        Err(LibraryError::NoGtSave { found }) => assert!(found.is_empty()),
        other => panic!("expected NoGtSave for an empty card, got {other:?}"),
    }
}

#[test]
fn export_writes_a_save_as_a_psu_file() {
    let library = Library::open(&scratch("export"), GT4).unwrap();
    library.harvest(&card_of(&[game_save(4)])).unwrap();
    let bytes = library.export_save_psu(DEFAULT_SAVE_NAME).unwrap();
    assert!(same(&psu::read(&bytes).unwrap(), &game_save(4)));
}

#[test]
fn rename_renames_the_folder_and_the_selection_follows() {
    let library = Library::open(&scratch("rename"), GT4).unwrap();
    library.harvest(&card_of(&[game_save(5)])).unwrap();
    library.import_items_named(&[game_save(6)], "Other").unwrap();
    assert_eq!(library.rename_save(DEFAULT_SAVE_NAME, "  My\tcareer  ").unwrap(), "My career");
    assert!(!library.save_dir(DEFAULT_SAVE_NAME).exists());
    assert!(library.has_game_data("My career"));
    assert_eq!(library.selected().unwrap().as_deref(), Some("My career"), "the selected save stays selected");
    // A name taken by another save gets a number; the same name in other letters is a plain rename.
    assert_eq!(library.rename_save("Other", "my CAREER").unwrap(), "my CAREER (2)");
    assert_eq!(library.rename_save("My career", "MY CAREER").unwrap(), "MY CAREER");
    assert_eq!(library.saves().unwrap(), ["MY CAREER", "my CAREER (2)"]);
    assert!(matches!(library.rename_save("MY CAREER", " \u{7} "), Err(LibraryError::Name(_))));
    assert!(matches!(library.rename_save("Missing", "x"), Err(LibraryError::NotFound(_))));
    // An empty save can be renamed too.
    let empty = library.new_save("Empty").unwrap();
    assert_eq!(library.rename_save(&empty, "Fresh start").unwrap(), "Fresh start");
    assert_eq!(library.selected().unwrap().as_deref(), Some("Fresh start"));
}

#[test]
fn a_removed_selected_save_selects_the_first_one_left_or_a_new_default() {
    let library = Library::open(&scratch("removed"), GT4).unwrap();
    library.import_items_named(&[game_save(1)], "B save").unwrap();
    library.import_items_named(&[game_save(2)], "A save").unwrap();
    library.select("B save").unwrap();
    fs::remove_dir_all(library.save_dir("B save")).unwrap();
    assert_eq!(library.ensure_selected().unwrap(), "A save");
    fs::remove_dir_all(library.save_dir("A save")).unwrap();
    fs::remove_dir_all(library.save_dir(DEFAULT_SAVE_NAME)).unwrap();
    assert_eq!(library.ensure_selected().unwrap(), DEFAULT_SAVE_NAME, "no save is left: a new empty default save");
    assert_eq!(library.saves().unwrap(), [DEFAULT_SAVE_NAME]);
}

/// The old layout: `Active` and `Save NN` slots, names in `name.txt`.
fn old_layout(root: &Path, active_name: Option<&str>) -> PathBuf {
    let saves = root.join("Saves");
    let active = saves.join(ACTIVE_FOLDER);
    fs::create_dir_all(&active).unwrap();
    folder::export_item(&active, &game_save(1)).unwrap();
    if let Some(name) = active_name {
        fs::write(active.join(NAME_FILE), name).unwrap();
    }
    for (slot, seed, name) in [("Save 01", 2, Some("gran-turismo-4.22070")), ("Save 02", 3, None)] {
        let folder_path = saves.join(slot);
        fs::create_dir_all(&folder_path).unwrap();
        folder::export_item(&folder_path, &game_save(seed)).unwrap();
        if let Some(name) = name {
            fs::write(folder_path.join(NAME_FILE), name).unwrap();
        }
    }
    saves
}

#[test]
fn the_old_layout_is_converted_once_and_keeps_every_save() {
    let root = scratch("convert");
    let saves = old_layout(&root, Some("My career"));
    let library = Library::open(&root, GT4).unwrap();
    assert_eq!(library.saves().unwrap(), ["gran-turismo-4.22070", "My career", "Save 02"]);
    assert_eq!(library.selected().unwrap().as_deref(), Some("My career"), "the active save stays the one Play uses");
    for (name, seed) in [("My career", 1), ("gran-turismo-4.22070", 2), ("Save 02", 3)] {
        assert!(same(&folder::import_item(&library.game_data_dir(name)).unwrap(), &game_save(seed)), "{name}");
        assert!(!library.save_dir(name).join(NAME_FILE).exists(), "the name is the folder now");
    }
    assert!(!saves.join(ACTIVE_FOLDER).exists());
    // Once converted, a save the player names "Active" is a save like any other.
    library.rename_save("Save 02", "Active").unwrap();
    let again = Library::open(&root, GT4).unwrap();
    assert_eq!(again.saves().unwrap(), ["Active", "gran-turismo-4.22070", "My career"]);
    assert_eq!(again.selected().unwrap().as_deref(), Some("My career"));
}

#[test]
fn an_unnamed_old_active_save_becomes_the_default_save() {
    let root = scratch("convert-unnamed");
    old_layout(&root, None);
    let library = Library::open(&root, GT4).unwrap();
    assert_eq!(library.saves().unwrap(), [DEFAULT_SAVE_NAME, "gran-turismo-4.22070", "Save 02"]);
    assert_eq!(library.active_save().unwrap().as_deref(), Some(DEFAULT_SAVE_NAME));
}

#[test]
fn an_old_interrupted_swap_is_finished_before_the_conversion() {
    // The old activate_slot stopped after moving the active save aside: Active is empty, the save waits in the swap folder.
    let root = scratch("convert-swap");
    let saves = root.join("Saves");
    let swap = saves.join(SWAP_FOLDER);
    fs::create_dir_all(&swap).unwrap();
    folder::export_item(&swap, &game_save(4)).unwrap();
    fs::write(swap.join(NAME_FILE), "Waiting").unwrap();
    fs::create_dir_all(saves.join(ACTIVE_FOLDER)).unwrap();
    let library = Library::open(&root, GT4).unwrap();
    assert_eq!(library.saves().unwrap(), ["Waiting"]);
    assert_eq!(library.selected().unwrap().as_deref(), Some("Waiting"));
    assert!(same(&folder::import_item(&library.game_data_dir("Waiting")).unwrap(), &game_save(4)));
    assert!(!swap.exists());

    // The swap had already moved the slot's save into Active: the waiting save becomes a save of its own.
    let root = scratch("convert-swap-half");
    let saves = root.join("Saves");
    let swap = saves.join(SWAP_FOLDER);
    fs::create_dir_all(&swap).unwrap();
    folder::export_item(&swap, &game_save(4)).unwrap();
    fs::write(swap.join(NAME_FILE), "Waiting").unwrap();
    let active = saves.join(ACTIVE_FOLDER);
    fs::create_dir_all(&active).unwrap();
    folder::export_item(&active, &game_save(5)).unwrap();
    fs::write(active.join(NAME_FILE), "Chosen").unwrap();
    let library = Library::open(&root, GT4).unwrap();
    assert_eq!(library.saves().unwrap(), ["Chosen", "Waiting"]);
    assert_eq!(library.selected().unwrap().as_deref(), Some("Chosen"));
    assert!(same(&folder::import_item(&library.game_data_dir("Waiting")).unwrap(), &game_save(4)));
    assert!(same(&folder::import_item(&library.game_data_dir("Chosen")).unwrap(), &game_save(5)));
}

#[test]
fn clean_name_drops_what_a_folder_name_cannot_hold() {
    assert_eq!(clean_name("a\tb").unwrap(), "a b");
    assert_eq!(clean_name("<>|?*\"/\\:x").unwrap(), "x");
    assert_eq!(clean_name("Corrida Fácil").unwrap(), "Corrida Fácil");
    assert_eq!(clean_name("..hidden.").unwrap(), "hidden", "a leading dot would hide the save");
    assert_eq!(clean_name("con").unwrap(), "con save", "Windows reserves CON");
    assert_eq!(clean_name("COM1.txt").unwrap(), "COM1.txt save");
    assert!(clean_name("  \u{7}  ").is_err());
    assert!(clean_name(" ... ").is_err());
    let long = "x".repeat(NAME_MAX_CHARS + 10);
    assert_eq!(clean_name(&long).unwrap().chars().count(), NAME_MAX_CHARS);
}
