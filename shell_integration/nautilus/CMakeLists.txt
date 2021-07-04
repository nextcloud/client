# copy the python nautilus plugin to build dir as it is going to
# be changed.
if( UNIX AND NOT APPLE )

    configure_file(syncstate.py syncstate.py COPYONLY)

    # Call the setupappname.sh script to set the custom app name.
    set (cmd "${CMAKE_CURRENT_SOURCE_DIR}/setappname.sh")
    execute_process(COMMAND ${cmd} ${APPLICATION_SHORTNAME}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    ERROR_VARIABLE errors OUTPUT_VARIABLE out)

    # Create a nemo plugin script from the nautilus one. 
    # nemocmd copies the syncstate.py and performs string replacement.
    set (nemocmd "${CMAKE_CURRENT_SOURCE_DIR}/createnemoplugin.sh")
    execute_process(COMMAND ${nemocmd}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    ERROR_VARIABLE errors OUTPUT_VARIABLE out)

    # Create a caja plugin script from the nautilus one. 
    # cajacmd copies the syncstate.py and performs string replacement.
    set (cajacmd "${CMAKE_CURRENT_SOURCE_DIR}/createcajaplugin.sh")
    execute_process(COMMAND ${cajacmd}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
                    ERROR_VARIABLE errors OUTPUT_VARIABLE out)

                                        
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate.py DESTINATION ${CMAKE_INSTALL_DATADIR}/nautilus-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate_nemo.py DESTINATION ${CMAKE_INSTALL_DATADIR}/nemo-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/syncstate_caja.py DESTINATION ${CMAKE_INSTALL_DATADIR}/caja-python/extensions RENAME syncstate-${APPLICATION_SHORTNAME}.py)
    
    

endif()

