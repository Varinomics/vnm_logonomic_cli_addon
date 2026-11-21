# Included after every project() of the builds tests/embedded_package_tests.cmake
# compares. MSVC stamps each linked image with the time of the link; /Brepro
# replaces the stamp with a hash of the image, so the same objects link to the
# same bytes.
if(MSVC)
    add_link_options(/Brepro)
endif()
