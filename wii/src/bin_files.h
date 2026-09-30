#ifndef AM2R_WII_BIN_FILES_H
#define AM2R_WII_BIN_FILES_H

#include "file_system.h"

/* file_bin_* files live in memory from open to close; the card sees one read and one write. */
void WiiBinFiles_install(FileSystem *fs);

#endif
