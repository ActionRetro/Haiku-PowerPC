/*
 * Copyright 2026, The Tabby Maintainers.
 * Distributed under the terms of the MIT License.
 */
#ifndef TABBY_VERSION_H
#define TABBY_VERSION_H


// Tabby's user-facing release version. Bump this by hand for each release.
#define TABBY_VERSION_RELEASE	"0.1"

// Optional codename shown after the release version (empty string = none).
#define TABBY_VERSION_CODENAME	""


// NOTE: Tabby's internal *build* number is not defined here. It is derived
// automatically at runtime from the git-based Haiku revision that
// determine_haiku_revision bakes into libroot at build time: the commit count
// since the base Haiku hrev tag equals Tabby's own commits on top of Haiku.
// See AboutSystem.cpp:_GetOSVersion(). The build number therefore bumps on
// every commit with no manual upkeep. (Caveat: if the fork is ever rebased
// onto a newer upstream hrev tag, the count restarts from that tag.)


#endif	// TABBY_VERSION_H
