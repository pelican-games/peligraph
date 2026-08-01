# qt target settings
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

# this must be before add_executable
qt_standard_project_setup(REQUIRES 6.10)
