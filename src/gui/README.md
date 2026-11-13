# GUI module

Reserved for a future graphical frontend. No GUI implementation or toolkit is selected yet.

Use the frontend-independent APIs in `include/log_viewer/core/` for source acquisition,
queries, search results, and log storage. Own GUI state, rendering, and event handling
here, and keep toolkit types out of core headers. Do not depend on the TUI or CLI
application modules. Add an explicit GUI build target when implementing this frontend;
the default executable currently builds only core, TUI, and CLI application sources.
