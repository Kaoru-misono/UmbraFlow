# UmbraFlow

A strictly-background personal game automation app: a C++ host observes a game
window, recognizes annotated pages, and delivers provable background actions;
task scripts written in Luau drive the loop through a minimal capability API.

## Language

### Scripting

**Capability namespace (`umbra`)**:
The single read-only global root through which a task script reaches every
host capability (observation, recognition, pages, actions, waiting). A script
sees nothing of the host outside this root.
_Avoid_: `bot` (superseded draft wording in the grill decisions and the S0
annotation design), `uf` (the C++ namespace, unrelated to the script surface)

**Task**:
One automation flow (such as a game's daily routine) authored as a Luau
script and executed by the host in its own isolated VM against one target
window. A task always belongs to exactly one project and is addressed by its
name within that project, never by a loose file path.
_Avoid_: script (the source artifact a task is written in, not the unit of
execution), job, macro

**Project**:
Everything the host needs to automate one game target: the authoring
document, source screenshots, the generated runtime recognition assets, and
the tasks written against them.
_Avoid_: workspace, profile
