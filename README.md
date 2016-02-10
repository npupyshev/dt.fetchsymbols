# dt.fetchsymbols
com.apple.dt.fetchsymbols client.
# build
xcrun -sdk macosx clang -F/System/Library/PrivateFrameworks -framework MobileDevice -framework CoreFoundation main.c -o fetchsymbols
# usage
fetchsymbols [Options]

Options:

  -l           -  List available files.
  
  -f n path    -  Download file with index n to path 'path'.
  
  -c path      -  Download dyld shared cache to path 'path'.

  -C arch path -  Download dyld shared cache for architecture 'arch' to path 'path'.
  
  -d path      -  Download /usr/lib/dyld to path 'path'.
  
  -h           -  Display this message.
