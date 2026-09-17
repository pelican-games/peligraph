# Build glue for one Pelican package.
#
# pelican_packages(), called from the host project's code/CMakeLists.txt, adds
# this directory.  pelican_package_sources() compiles these files against the
# public SDK only and merges the objects into the project's game logic DLL --
# a package never produces a DLL of its own.
#
# Relative paths resolve against this directory.
pelican_package_sources(system.cpp)
