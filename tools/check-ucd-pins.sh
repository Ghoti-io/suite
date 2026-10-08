#!/bin/sh
#
# Fail if two generated-table pins name different versions of the Unicode
# Character Database.
#
# Three libraries here generate tables from the UCD, and until they are
# consolidated onto `unicode` each has its own pin - four files, because `text`
# pins the UCD and the IDNA mapping table separately. Nothing checks that they
# agree, and a disagreement is the kind of defect that produces two answers for
# one question: `text` normalising with one version's composition exclusions
# while `regex` matches `\p{...}` from another's, in one process, with no error
# anywhere. Two pins inside one library do it just as well.
#
# They agree today by coincidence - all four read 17.0.0 - which is exactly
# when a check is worth adding, because the coincidence is what a reviewer
# would otherwise rely on.
#
# After libs/unicode/documentation/design.md's phases D and E, `text` and
# `regex` have no pin of their own and this check is trivially true over one
# file. That is the point of it: it goes from "the three agree" to "there is
# one", and the day it stops finding three files is the day the consolidation
# finished.
#
# Usage: suite/tools/check-ucd-pins.sh
#
# Copyright 2026 by Corey Pennycuff

set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)

found=0
mismatch=0
first_version=""
first_file=""

# Every pin, wherever a library keeps it. Found rather than listed: a library
# that grows one should be checked without this script being edited.
#
# `*_VERSION` rather than `UCD_VERSION`, because the first version of this
# script matched the exact name and `text` keeps two pins side by side in
# tools/idna/: UCD_VERSION and IDNA_MAPPING_VERSION. IdnaMappingTable.txt is
# published per Unicode version, so the two have to name the same one - and
# while they did not, from this script's f9932de until 2026-09-26, it printed
# "3 Unicode pins, all 17.0.0" over a tree where one library read 17.0.0 for
# its properties and 16.0.0 for its IDNA mapping. Green over exactly the state
# it exists to refuse, because the population was named by a filename.
pins=$(find "$root/libs" -name '*_VERSION' -not -path '*/third_party/*' | sort)

if [ -z "$pins" ]; then
  printf 'check-ucd-pins: no *_VERSION file anywhere under libs/.\n' >&2
  printf 'That is not a pass: it means this check could not see anything.\n' >&2
  exit 1
fi

# What was found and deliberately not treated as a pin, printed rather than
# dropped: the denominator is the part of a check like this that goes wrong, so
# a reader can see the files it skipped and say whether that is still right.
# Today these are the reference manifests - `image`'s and `regex`'s VERSIONS -
# which pin decoders, corpora and interpreters. Those carry Unicode versions of
# their own and are *allowed* to differ from these tables: an oracle behind the
# pin is a documented skew, not two answers to one question.
others=$(find "$root/libs" -name '*VERSION*' -not -name '*_VERSION' \
    -not -path '*/third_party/*' | sort)

for pin in $pins; do
  version=$(tr -d ' \t\n\r' < "$pin")
  relative=${pin#"$root"/}
  if [ -z "$version" ]; then
    printf 'check-ucd-pins: %s is empty\n' "$relative" >&2
    mismatch=1
    continue
  fi
  found=$((found + 1))
  if [ -z "$first_version" ]; then
    first_version=$version
    first_file=$relative
  elif [ "$version" != "$first_version" ]; then
    printf 'check-ucd-pins: %s pins %s and %s pins %s\n' \
        "$first_file" "$first_version" "$relative" "$version" >&2
    mismatch=1
  fi
  printf '  %-44s %s\n' "$relative" "$version"
done

if [ "$mismatch" -ne 0 ]; then
  printf '\n\033[0;31m### Two pins name different Unicode versions ###\033[0m\n' >&2
  printf '\nOne process reading both gets two answers for one question - a\n' >&2
  printf 'normalisation from one version and a property from another - with no\n' >&2
  printf 'error anywhere, and two pins in one library do that without two\n' >&2
  printf 'libraries being involved. Regenerate the tables of whichever is\n' >&2
  printf 'behind, or finish the migration that removes its pin.\n' >&2
  exit 1
fi

if [ -n "$others" ]; then
  printf '\nNot treated as table pins (reference manifests, allowed to differ):\n'
  for other in $others; do
    printf '  %s\n' "${other#"$root"/}"
  done
fi

printf '\n\033[0;32m%d Unicode pins, all %s.\033[0m\n' "$found" "$first_version"
if [ "$found" -eq 1 ]; then
  printf 'One pin left: the consolidation is finished.\n'
fi
