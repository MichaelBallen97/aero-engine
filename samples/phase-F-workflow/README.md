# Phase F gate artifact — the workflow project

**Gate (docs/tasks/phase-F.md):** *Open the editor and it fills the screen; drag a model in from the file manager and
it imports; click the object under the cursor, even one standing behind another object's bounding box, and delete it
with the Delete key; select a camera and see what it sees; create a script and a second scene from the browser and
switch scenes from the breadcrumb; save a script while the game plays and see the change at once.*

**Artifact:** an Aero project committed in this folder — two scenes, an imported model with a cube standing behind
its bounding box, a camera, and a script. It is data, not a build target: nothing here is `add_subdirectory`'d.

F.1.1 commits the project — a TypeScript project, with a model file kept outside it to drag in — and every story's demo
starts from it; this page states what it has to show.

## The gate as a scenario

- Given the editor was last closed full screen, or has not run before, when I start it, then its window fills the
  screen.
- Given this project is open, when I drag a model file from the file manager onto the Assets panel, then it is copied
  into the folder shown and imports.
- Given the cube standing behind the model, when I click a pixel of the cube, then the cube is selected and not the
  model whose bounding box covers it, and when I press Delete, then the cube is removed and one Undo brings it back.
- Given the camera, when I select it, then the viewport shows its frustum and what it sees.
- Given the Assets panel, when I create a script and a second scene from it, then both exist, and when I click the
  scene's name in the breadcrumb, then I can switch between the two scenes.
- Given the game is playing, when I save a change to the script, then the running game follows it at once.

## How the demo is built up

Phase F is cut into four stories, in the `## Stories` section of `docs/tasks/phase-F.md`. Each story makes more of
the scenario run, and each one's demo starts from this folder and takes about two minutes:

1. **Open, drop, click, delete.** The editor opens full screen, a model dragged in from the file manager imports, a
   click selects exactly the object under the cursor, and Delete removes it.
2. **A viewport that reads.** One click selects a whole model and a second its part, the view-axis widget draws its
   axes, a selected camera shows its frustum and its view, and the view options live in the View menu.
3. **Scripts and scenes in the browser.** New Script and New Scene are created where the assets are, a scene opens on
   a double-click and from the breadcrumb's list, each scene shows a picture, and the panel that edits the selection
   comes forward by itself.
4. **Save and see.** A saved script, material or model reaches the running game within the recorded budget.
