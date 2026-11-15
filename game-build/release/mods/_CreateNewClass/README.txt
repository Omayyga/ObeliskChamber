Create New Class 1.0
A mod for Obelisk Chamber (Fire Emblem: Fortune's Weave on PC).
Make your own classes inside the game, from the Class Change screen.

This file is the short guide. DOCUMENTATION.txt has every detail.


READ THIS FIRST
  A saved game remembers each unit's class. If a unit is in a class you
  made and that class is missing (the mod is removed, or its job.txt is
  lost), the game crashes when it loads that save - at start-up, because
  the game reads your last save before the title screen.

  So:
    - Keep one save slot that has no units in created classes.
    - Never delete the CreateNewClass folder to update the mod. Your
      classes are the files job.txt and asset.txt inside it.
    - Before removing a class, move its units to another class and save.


INSTALL
  In an Obelisk Chamber release the mod is already there, switched off:
  mods\_CreateNewClass. A folder whose name starts with _ is not loaded.
  1. Rename the folder to CreateNewClass (take the _ away).
  2. Start the game. The mod writes job.txt with empty placeholders.
  On its own: copy the CreateNewClass folder into the game's mods folder.
  Needs an Obelisk Chamber build with mod interface version 4.

UPDATE
  Copy the new folder's files over your CreateNewClass folder and let
  them replace files. (A new Obelisk Chamber release brings the mod as
  mods\_CreateNewClass again; your CreateNewClass folder is left alone,
  so copy exefs\create_class.dll from the new one into yours.) The download has no job.txt or asset.txt, so your classes stay.
  The mod also keeps job.txt.bak and asset.txt.bak and puts them back if
  job.txt is ever missing or empty again.


MAKE A CLASS
  1. Open the Class Change screen for a unit.
  2. Click the left stick. The list of your classes opens.
  3. Choose "New class".
  4. Choose the kind of class (Infantry, Magic, Armored, Cavalry, Fliers),
     then the Class Tier, then the class to start from (the Base Class).
     For a class that rides, pick from Cavalry or Fliers.
  5. Answer Yes on the box. The lists close and the new class opens.
  6. Change what you like (see below).
  7. Restart the game when a line says "after a restart".

  While you move through the look choices, the unit on the Class Change
  screen shows the line under the cursor. Confirm to keep it; back out to
  leave it as it was.


WHAT YOU CAN CHANGE
  Base Class             start again from another class
  Name                   the name and the description, typed on the
                         game's keyboard                    (restart)
  Class Info             Class Tier and Movement, Class Type, Gender,
                         Class Icon, Class Line (previous / next class)
  Basic Stats            Basic Stats, Class Bonus, Growth Rate, Growth
                         Rate Bonus
  Skill Exp Bonus        the thirteen skills
  Abilities              Class Ability 1-3                  (restart)
                         Master Ability
  Certification Exam     Primary and Secondary Skills, Pass Chance,
                         Barred characters
  Mounted                Generate / Regenerate dismounted class
                                                            (restart)
                         Mount model, Dismounted Class Bonus
  Appearance             Body, Material, Outfit Colors, Map scale,
                         Accessories, Footsteps
  Animations and Magic   animations per weapon; casting     (restart)
  Remove this class      asks first; gone after a restart

  Move with the d-pad or the left stick (the right stick stays the
  camera). Numbers: left / right change a value, confirm takes a bigger
  step. Lists: confirm chooses and goes back.


GOOD TO KNOW
  - Changes are saved as you make them. Nothing else to press.
  - There are always a few empty placeholders, so "New class" is always
    there. More are added by themselves at the next start.
  - A mounted class needs a mounted Base Class. "Generate dismounted
    class" then gives it an on-foot version of its own. That one follows
    the mounted class in everything; only its Class Bonus is set
    separately (Mounted > Dismounted Class Bonus).
  - Enemies do not use created classes.


SHARING A CLASS
  A class is its entry in job.txt plus its entry in asset.txt, so the two
  entries can be copied into someone else's files by hand (see
  DOCUMENTATION.txt, "The files"). Rebuilding the class in the game's
  menu from a list of its settings is the safer way: nothing is left out
  and nothing clashes with classes already there.


IF SOMETHING GOES WRONG
  The game crashes at start-up after an update or after removing a class
      A save has a unit in a class that no longer exists. Put job.txt
      and asset.txt back (from the .bak files next to them, or from your
      own copy) and start again.
  A class says "not loaded (restart the game)"
      Restart the game.
  A change does not show
      Lines marked (restart) above need a restart.
  ObeliskChamber.log in the game's run folder says what the mod did; its
  lines start with "CreateNewClass". For a report, set debug = true in
  job.ini first: the log then has every step.
