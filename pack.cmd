/* pack.cmd to package a project using GNU Make/GCC build system and git */

call setlocal

/****** Configuration parts begin ******/

sPackageName = 'kmididec'
sRepoDir = '.'

sVerMacro = 'KMIDIDEC_VERSION'
sVerHeader = 'kmididec.h'

sDistFiles = 'kmidi.exe' '/',
             'kmidimmio.exe' '/',
             'README' '/',
             'kmididec.h' '/include',
             'kmidide0.dll' '/lib',
             'kmididec.a' '/lib',
             'kmididec.lib' '/lib',
             'kmididec_dll.a' '/lib',
             'kmididec_dll.lib' '/lib'


/****** Configuration parts end ******/

'echo' 'on'

sCmd = 'sed -n "s/^#define' sVerMacro '*\\\"\(.*\)\\\"$/\1/p"' sVerHeader
sVer = getOutput( sCmd )
sShortVer = removeNonNumbers( sVer )
sPackageNameVer = sPackageName || '-' || sVer

'gmake' 'clean'
'gmake' 'RELEASE=1'

'sed' '-e' 's/@VER@/' || sVer || '/g',
      '-e' 's/@SHORT_VER@/' || sShortVer || '/g',
       sPackageName || '.txt' '>' sPackageNameVer || '.txt'

sDistFiles = sDistFiles,
             sPackageNameVer || '.txt' '/.'

'mkdir' sPackageNameVer

do while strip( sDistFiles ) \= ''
    parse value sDistFiles with sSrc sDestDir sDistFiles

    'ginstall' '-d' sPackageNameVer || sDestDir
    'ginstall' sRepoDir || '/' || sSrc sPackageNameVer || sDestDir
end

'git' 'archive' '--format' 'zip' sVer '--prefix' sPackageNameVer || '/' '>',
      sPackageNameVer || '/' || sPackageNameVer || '-src.zip'

'rm' '-f' sPackageNameVer || '.zip'
'zip' '-rpSm' sPackageNameVer || '.zip' sPackageNameVer

call endlocal

exit 0

/* Get outputs from commands */
getOutput: procedure
    parse arg sCmd

    nl = x2c('d') || x2c('a')

    rqNew = rxqueue('create')
    rqOld = rxqueue('set', rqNew )

    address cmd sCmd '| rxqueue' rqNew

    sResult = ''
    do while queued() > 0
        parse pull sLine
        sResult = sResult || sLine || nl
    end

    call rxqueue 'Delete', rqNew
    call rxqueue 'Set', rqOld

    /* Remove empty lines at end */
    do while right( sResult, length( nl )) = nl
        sResult = delstr( sResult, length( sResult ) - length( nl ) + 1 )
    end

    return sResult

/* Remove non-number characters */
removeNonNumbers: procedure
    parse arg sStr

    do i = length( sStr) to 1 by -1
        if datatype( substr( sStr, i, 1 ), 'n') = 0  then
            sStr = delstr( sStr, i, 1 )
    end

    return sStr
