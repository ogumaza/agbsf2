// SPDX-License-Identifier: MIT

// The builds of Krawall that agbsf2 reads.

#pragma once

namespace agbsf2::krawall
{

// The builds of the library, oldest first. A signed build has the date of its version string. A later build is an
// earlier one with changes. The player therefore tests whether a build comes before another. The unsigned and modified
// builds come last, and code that treats them differently names them.
enum class Build
{
    kV1_2_2_10,  // $Id: player.c,v 1.2.2.10 2002/08/24
    kV1_2_2_14,  // $Id: player.c,v 1.2.2.14 2002/09/12
    kV1_2_2_15,  // $Id: player.c,v 1.2.2.15 2002/12/19
    kV1_2_2_17,  // $Id: player.c,v 1.2.2.17 2003/01/13
    kV1_4,       // $Id: player.c,v 1.4 2003/03/19
    k2003_09_01, // $Id: Krawall $Date: 2003/09/01
    k2004_09_17, // $Id: Krawall $Date: 2004/09/17
    k2004_11_14, // $Id: Krawall $Date: 2004/11/14
    k2005_04_21, // $Id: Krawall $Id: version.h 8 2005-04-21
    kUnsigned,   // a build without a version string, with the data and records of the builds from 2004/09/17 on
    kModified,   // a game developer's change to the build of 2005-04-21, with modules in packed music files
};

// Returns the build's name, for --info and messages.
const char* BuildName(Build build);

} // namespace agbsf2::krawall
