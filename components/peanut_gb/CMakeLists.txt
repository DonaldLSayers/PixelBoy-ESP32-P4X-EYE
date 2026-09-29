# Vendored single-header Game Boy emulator core (MIT, deltabeard/Peanut-GB).
# Header-only - the implementation is compiled wherever it's #included
# (app_gbemu.c, exactly once), so there's nothing to build here.
idf_component_register(INCLUDE_DIRS "include")
