find_program(RPMBUILD rpmbuild)

if (RPMBUILD)
    find_program(MKDIR mkdir)
    find_program(CP cp)
    find_program(WC wc)

    execute_process (COMMAND ${GIT} describe HEAD --abbrev=0
        OUTPUT_VARIABLE VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    execute_process (COMMAND ${GIT} rev-list --oneline ${VERSION}..
        COMMAND ${WC} -l
        OUTPUT_VARIABLE RELEASE
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    execute_process (COMMAND ${RPMBUILD} -E '%_srcrpmdir'
        OUTPUT_VARIABLE RPM_SRCRPMDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    execute_process (COMMAND ${RPMBUILD} -E '%_sourcedir'
        OUTPUT_VARIABLE RPM_SOURCEDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    set (RPM_PACKAGE_VERSION ${VERSION} CACHE STRING "" FORCE)
    set (RPM_PACKAGE_RELEASE ${RELEASE} CACHE STRING "" FORCE)

    set (RPM_SOURCE_DIRECTORY_NAME ${CPACK_SOURCE_PACKAGE_FILE_NAME}
        CACHE STRING "" FORCE)
    set (RPM_PACKAGE_SOURCE_FILE_NAME ${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        CACHE STRING "" FORCE)

    set (RPM_BUILDROOT "${PROJECT_BINARY_DIR}/RPM/BUILDROOT" CACHE STRING "" FORCE)

    add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        COMMAND $(MAKE) package_source)

    add_custom_command(OUTPUT ${RPM_BUILDROOT}
        COMMAND ${MKDIR} -p ${RPM_BUILDROOT})

    add_custom_command(OUTPUT ${RPM_SOURCEDIR}
        COMMAND ${MKDIR} -p ${RPM_SOURCEDIR})

    ##############################################################################################

    set (RPM_ROOT "${PROJECT_BINARY_DIR}/RPM" CACHE STRING "" FORCE)

    add_custom_command(OUTPUT ${RPM_ROOT}
        COMMAND ${MKDIR} -p ${RPM_ROOT}/{BUILD,SOURCES,SRPMS} ${RPM_ROOT}/RPMS/{i386,x86_64})

    add_custom_target(rpm
        DEPENDS ${RPM_ROOT}
        DEPENDS ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        COMMAND ${CP} ${PROJECT_BINARY_DIR}/${RPM_PACKAGE_SOURCE_FILE_NAME} ${RPM_ROOT}/SOURCES
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} -bb ${PROJECT_SOURCE_DIR}/extra/rpm.spec
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    add_custom_target(rpm_src
        DEPENDS ${RPM_ROOT}
        DEPENDS ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        COMMAND ${CP} ${PROJECT_BINARY_DIR}/${RPM_PACKAGE_SOURCE_FILE_NAME} ${RPM_ROOT}/SOURCES
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} -bs ${PROJECT_SOURCE_DIR}/extra/rpm.spec
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    ##############################################################################################

    add_custom_target(new_rpm
        DEPENDS ${RPM_BUILDROOT}
        DEPENDS ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} --define '_sourcedir ./' -bb ${PROJECT_SOURCE_DIR}/extra/rpm/tarantool.rpm.spec
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    add_custom_target(new_rpm_src
        DEPENDS ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} --define '_sourcedir ./' --define '_srcrpmdir ./' -bs ${PROJECT_SOURCE_DIR}/extra/rpm/tarantool.rpm.spec
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    add_custom_target(new_rpm_scl
        DEPENDS ${RPM_BUILDROOT}
        DEPENDS ${PROJECT_BINARY_DIR}/${CPACK_SOURCE_PACKAGE_FILE_NAME}.tar.gz
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} -bb ${PROJECT_SOURCE_DIR}/extra/rpm/tarantool-scl.rpm.spec
        COMMAND ${RPMBUILD} --buildroot ${RPM_BUILDROOT} --define '_sourcedir ./' -bb ${PROJECT_SOURCE_DIR}/extra/rpm/tarantool.rpm.spec --define 'scl 15'
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR})

    # TODO: Add MOCK builds
    #     : -DMOCK_TARGET
    #     : -DMOCK_OS: EPEL / FEDORA

endif()
