// Library rules for the Saves page (TASK-104): New save, Export of the active save, Rename and the name kept with a save.

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
use gt4saves::library::{clean_name, LibraryError, NAME_MAX_CHARS};
use gt4saves::{psu, Card, Library};
use std::fs;
use std::path::PathBuf;

fn scratch(name: &str) -> PathBuf {
    let path = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(name);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn card_of(items: &[Node]) -> Vec<u8> {
    Card::build(items).unwrap().to_image()
}

fn game_names(bytes: &[u8]) -> Vec<String> {
    Card::from_image(bytes).unwrap().root().unwrap().iter().map(Node::name).collect()
}

/// A library whose active save is `seed`'s save, after an import and an activation.
fn library_with_active(name: &str, seed: u8) -> Library {
    let library = Library::open(&scratch(name), GT4).unwrap();
    library.import_card_image(&card_of(&[game_save(seed)])).unwrap();
    library.activate_slot("Save 01").unwrap();
    library
}

#[test]
fn new_save_parks_the_active_save_and_the_next_card_has_no_game_save() {
    let library = library_with_active("new-save", 1);
    assert_eq!(library.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));

    let parked = library.new_save().unwrap();
    assert_eq!(parked.as_deref(), Some("Save 01"));
    assert_eq!(library.active_save().unwrap(), None);
    assert_eq!(library.slots().unwrap(), ["Save 01"]);

    // Play with an empty Active builds a card with the replays only.
    let card = library.build_card(&[]).unwrap();
    assert!(!game_names(&card).contains(&GT4_GAME_SAVE.to_string()));
    let library_replay = replay(7);
    let with_replay = Card::build(&[library_replay.clone()]).unwrap().to_image();
    library.import_card_image(&with_replay).unwrap();
    let card = library.build_card(&["BASCUS-97328R0007".to_string()]).unwrap();
    assert_eq!(game_names(&card), ["BASCUS-97328R0007"]);
}

#[test]
fn new_save_with_no_active_save_parks_nothing() {
    let library = Library::open(&scratch("new-save-empty"), GT4).unwrap();
    assert_eq!(library.new_save().unwrap(), None);
    assert!(library.slots().unwrap().is_empty());
}

#[test]
fn harvest_fills_an_empty_active_save_and_parks_nothing_else() {
    let library = library_with_active("harvest-after-new", 1);
    library.new_save().unwrap();
    // The game made a new profile and wrote the card with it.
    let written = card_of(&[game_save(7)]);
    let report = library.harvest(&written).unwrap();
    assert_eq!(report.added, [GT4_GAME_SAVE.to_string()]);
    assert!(!report.replaced_active, "nothing was replaced: Active was empty");
    assert_eq!(library.active_save().unwrap().as_deref(), Some(GT4_GAME_SAVE));
    assert_eq!(library.slots().unwrap(), ["Save 01"], "the parked save is still there");
    // A harvest of a card with no game save changes nothing.
    let empty = card_of(&[]);
    let report = library.harvest(&empty).unwrap();
    assert!(report.added.is_empty());
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
fn export_writes_the_active_save_as_a_psu_file() {
    let library = library_with_active("export-active", 2);
    let bytes = library.export_psu(GT4_GAME_SAVE).unwrap();
    let node = psu::read(&bytes).unwrap();
    assert_eq!(node.name(), GT4_GAME_SAVE);
    assert!(same_contents(std::slice::from_ref(&node), &[game_save(2)]));
    library.new_save().unwrap();
    assert!(matches!(library.export_psu(GT4_GAME_SAVE), Err(LibraryError::NotFound(_))));
}

#[test]
fn rename_sets_a_clean_name_and_keeps_the_folder_name() {
    let library = library_with_active("rename", 1);
    library.new_save().unwrap();
    let stored = library.rename_save("Save 01", "  My   save: #1  ").unwrap();
    assert_eq!(stored, "My save #1");
    assert_eq!(library.display_name("Save 01").as_deref(), Some("My save #1"));
    assert!(library.slot_dir("Save 01").join(GT4_GAME_SAVE).is_dir(), "the folder name is unchanged");

    let long = "x".repeat(60);
    let stored = library.rename_save("Save 01", &long).unwrap();
    assert_eq!(stored.chars().count(), NAME_MAX_CHARS);

    assert!(matches!(library.rename_save("Save 01", " :: "), Err(LibraryError::Name(_))));
    assert!(matches!(library.rename_save("Save 09", "Nope"), Err(LibraryError::NotFound(_))));
}

#[test]
fn the_active_save_can_be_renamed() {
    let library = library_with_active("rename-active", 1);
    library.rename_save(GT4_GAME_SAVE, "Race").unwrap();
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Race"));
}

#[test]
fn a_name_goes_with_its_save_when_saves_are_swapped() {
    let library = library_with_active("names-swap", 1);
    library.rename_save(GT4_GAME_SAVE, "Alpha").unwrap();
    library.new_save().unwrap(); // Alpha parks as Save 01
    assert_eq!(library.display_name("Save 01").as_deref(), Some("Alpha"));
    library.import_card_image(&card_of(&[game_save(5)])).unwrap(); // Save 02, unnamed
    library.rename_save("Save 02", "Beta").unwrap();
    library.activate_slot("Save 02").unwrap(); // Beta becomes active; the empty Active had no name
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Beta"));
    library.activate_slot("Save 01").unwrap(); // Alpha back; Beta takes Alpha's slot folder with its name (TASK-074)
    assert_eq!(library.display_name(GT4_GAME_SAVE).as_deref(), Some("Alpha"));
    assert_eq!(library.display_name("Save 01").as_deref(), Some("Beta"));
    assert_eq!(library.slots().unwrap(), ["Save 01"]);
}

/// TASK-074: making a slot active swaps it with the active save in place: the slot numbers never climb, each save keeps
/// its contents and its name, however often the player switches.
#[test]
fn switching_back_and_forth_keeps_the_slot_numbers_and_each_save() {
    let library = library_with_active("swap-in-place", 1);
    library.rename_save(GT4_GAME_SAVE, "Career").unwrap();
    library.import_card_image(&card_of(&[game_save(2)])).unwrap();
    library.rename_save("Save 01", "Imported").unwrap();
    for round in 0..5 {
        library.activate_slot("Save 01").unwrap();
        assert_eq!(library.slots().unwrap(), ["Save 01"], "round {round}");
        let active = folder_contents(&library.active_dir().join(GT4_GAME_SAVE));
        let parked = folder_contents(&library.slot_dir("Save 01").join(GT4_GAME_SAVE));
        let (active_name, parked_name) = (library.display_name(GT4_GAME_SAVE).unwrap(), library.display_name("Save 01").unwrap());
        let (expected_active, expected_parked) = if round % 2 == 0 { (2, 1) } else { (1, 2) };
        assert!(same_contents(&[active], &[game_save(expected_active)]), "round {round}");
        assert!(same_contents(&[parked], &[game_save(expected_parked)]), "round {round}");
        assert_eq!((active_name.as_str(), parked_name.as_str()), if round % 2 == 0 { ("Imported", "Career") } else { ("Career", "Imported") });
    }
    assert!(!library.paths().saves.join(gt4saves::library::SWAP_FOLDER).exists());
}

fn folder_contents(folder: &std::path::Path) -> Node {
    gt4saves::folder::import_item(folder).unwrap()
}

/// TASK-074: a swap cut short is finished or undone at the next open, and no save is lost.
#[test]
fn an_interrupted_swap_is_repaired_when_the_library_opens() {
    use gt4saves::library::SWAP_FOLDER;
    // Cut after the active save went aside: Active is empty, so it comes back to Active.
    let early = library_with_active("swap-cut-early", 1);
    early.rename_save(GT4_GAME_SAVE, "Career").unwrap();
    let swap = early.paths().saves.join(SWAP_FOLDER);
    fs::create_dir_all(&swap).unwrap();
    fs::rename(early.active_dir().join(GT4_GAME_SAVE), swap.join(GT4_GAME_SAVE)).unwrap();
    fs::rename(early.active_dir().join("name.txt"), swap.join("name.txt")).unwrap();
    let reopened = Library::open_paths(early.paths(), GT4).unwrap();
    assert!(same_contents(&[folder_contents(&reopened.active_dir().join(GT4_GAME_SAVE))], &[game_save(1)]));
    assert_eq!(reopened.display_name(GT4_GAME_SAVE).as_deref(), Some("Career"));
    assert!(!swap.exists());

    // Cut after the slot's save moved in: the old active save is parked in a new slot.
    let late = library_with_active("swap-cut-late", 1);
    late.import_card_image(&card_of(&[game_save(2)])).unwrap(); // Save 01
    let swap = late.paths().saves.join(SWAP_FOLDER);
    fs::create_dir_all(&swap).unwrap();
    fs::rename(late.active_dir().join(GT4_GAME_SAVE), swap.join(GT4_GAME_SAVE)).unwrap();
    fs::rename(late.slot_dir("Save 01").join(GT4_GAME_SAVE), late.active_dir().join(GT4_GAME_SAVE)).unwrap();
    let reopened = Library::open_paths(late.paths(), GT4).unwrap();
    assert!(same_contents(&[folder_contents(&reopened.active_dir().join(GT4_GAME_SAVE))], &[game_save(2)]));
    let slots = reopened.slots().unwrap();
    assert_eq!(slots.len(), 1, "{slots:?}");
    assert!(same_contents(&[folder_contents(&reopened.slot_dir(&slots[0]).join(GT4_GAME_SAVE))], &[game_save(1)]));
    assert!(!swap.exists());
}

/// TASK-074: every save gets a name once; names that are already set stay, a taken name gets a number.
#[test]
fn unnamed_saves_get_a_name_once() {
    let library = library_with_active("name-unnamed", 1);
    library.import_card_image(&card_of(&[game_save(2)])).unwrap(); // Save 01
    library.import_card_image(&card_of(&[game_save(3)])).unwrap(); // Save 02
    library.rename_save("Save 02", "Default save").unwrap();
    let written = library.name_unnamed_saves("Default save").unwrap();
    assert_eq!(written, vec![
        (GT4_GAME_SAVE.to_string(), "Default save (2)".to_string()),
        ("Save 01".to_string(), "Save 01".to_string()),
    ]);
    assert!(library.name_unnamed_saves("Default save").unwrap().is_empty(), "names are written once");
    assert_eq!(library.display_name("Save 02").as_deref(), Some("Default save"));

    // An empty active save (the save the game creates at the next Play) is named too, and keeps the name.
    let empty = Library::open(&scratch("name-empty-active"), GT4).unwrap();
    assert_eq!(empty.name_unnamed_saves("Default save").unwrap(), vec![(GT4_GAME_SAVE.to_string(), "Default save".to_string())]);
    assert_eq!(empty.display_name(GT4_GAME_SAVE).as_deref(), Some("Default save"));
    empty.rename_save(GT4_GAME_SAVE, "My career").unwrap();
    assert_eq!(empty.display_name(GT4_GAME_SAVE).as_deref(), Some("My career"));
}

#[test]
fn clean_name_drops_what_a_file_name_cannot_hold() {
    assert_eq!(clean_name("a\tb").unwrap(), "a b");
    assert_eq!(clean_name("<>|?*\"/\\:x").unwrap(), "x");
    assert_eq!(clean_name("Corrida Fácil").unwrap(), "Corrida Fácil");
    assert!(clean_name("  \u{7}  ").is_err());
}
