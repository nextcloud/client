if( UNIX AND NOT APPLE )

    SET(ICON_DIR ${CMAKE_INSTALL_DATADIR}/icons/hicolor)
    
    FOREACH(size 128x128 16x16 256x256 32x32 48x48 64x64 72x72)
	file(GLOB files  "${size}/*.png")
	FOREACH( file ${files} )
	    # the GLOB returns a absolute path. Make it relative by replacing the current src dir by nothing
	    STRING(REPLACE "${CMAKE_CURRENT_SOURCE_DIR}/${size}/" "" shortFile ${file})
	    STRING(REPLACE "oC" ${APPLICATION_SHORTNAME} brandedName ${shortFile})
	    install(FILES ${file} DESTINATION ${ICON_DIR}/${size}/apps RENAME ${brandedName})
	ENDFOREACH(file)
    ENDFOREACH(size)

endif()
