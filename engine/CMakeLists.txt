# engine/ — 5-platform layer (docs/03). Subsystems in dependency order; each is added
# by its owning task (core: 0.2.1/0.2.2, platform: 0.3.1, rhi: 0.4.1, render: 0.5.1, ...).
# GOLDEN RULE: nothing in this tree may ever depend on editor/.

add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)        # task 0.4.1
