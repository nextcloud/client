#!//usr/bin/env python2.7

import subprocess
import re
import sys
import datetime
import os
import ConfigParser

distribution="yakkety"

versionTagRE = re.compile("^v([0-9]+((\.[0-9]+)+))(-(.+))?$")

def processVersionTag(tag):
    m = versionTagRE.match(tag)
    if m:
        return (m.group(1), "release" if m.group(4) is None else "beta")
    else:
        return None

def getCommitVersion(commit):
    major=None
    minor=None
    patch=None
    for line in subprocess.check_output(["git", "show",
                                         commit + ":CMakeLists.txt"]).splitlines():
        m = re.match("set\( MIRALL_VERSION_([A-Z]+) +([0-9])+ *\)", line)
        if m is not None:
            kind=m.group(1)
            version=m.group(2)
            if kind=="MAJOR":
                major=version
            elif kind=="MINOR":
                minor=version
            elif kind=="PATCH":
                patch=version
    if major and minor and patch:
        return major + "." + minor + "." + patch
    else:
        return None

def collectEntries(baseCommit, baseVersion, kind):
    scriptdir = os.path.dirname(__file__)
    configPath = os.path.join(scriptdir, "git2changelog.cfg")

    newVersionCommit = None
    newVersionTag = None
    newVersionOrigTag = None

    if os.path.exists(configPath):
        config = ConfigParser.SafeConfigParser()
        config.read(configPath)
        if config.has_section("versionhack"):
            if config.has_option("versionhack", "commit") and \
               config.has_option("versionhack", "tag"):
                newVersionCommit = config.get("versionhack", "commit")
                newVersionTag = config.get("versionhack", "tag")

    entries = []

    args = ["git", "log",
            "--format=%h%x09%an%x09%ae%x09%aD%x09%ad%x09%s",
            "--date=unix", "--author-date-order", "--reverse"]
    try:
        output = subprocess.check_output(args + [baseCommit + ".."])
    except:
        output = subprocess.check_output(args)


    lastVersionTag = None
    lastCMAKEVersion = None
    for line in output.splitlines():
        words = line.split("\t")
        (commit, name, email, date, revdate) = words[0:5]
        subject = "\t".join(words[5:])
        revdate = datetime.datetime.utcfromtimestamp(long(revdate)).strftime("%Y%m%d.%H%M%S")

        kind = "beta"

        if commit==newVersionCommit:
            result = processVersionTag(newVersionTag)
            if result:
                newVersionOrigTag = lastVersionTag
                (baseVersion, _kind) = result


        version=getCommitVersion(commit)
        if version and version!=lastCMAKEVersion:
            tag = "v" + version
            if tag!=newVersionOrigTag:
                result = processVersionTag(tag)
                if result:
                    lastVersionTag = tag
                    lastCMAKEVersion = version
                    (baseVersion, _kind) = result

        for tag in subprocess.check_output(["git", "tag",
                                            "--points-at",
                                            commit]).splitlines():
            if tag!=newVersionOrigTag:
                result = processVersionTag(tag)
                if result:
                    lastVersionTag = tag
                    (baseVersion, kind) = result


        entries.append((commit, name, email, date, revdate, subject,
                        baseVersion, kind))

    entries.reverse()

    return entries

def genChangeLogEntries(f, entries, distribution):
    latestBaseVersion = None
    latestKind = None
    for (commit, name, email, date, revdate, subject, baseVersion, kind) in entries:
        if latestBaseVersion is None:
            latestBaseVersion = baseVersion
            latestKind = kind
        upstreamVersion = baseVersion + "-" + revdate
        if distribution=="stable":
            version = upstreamVersion
        else:
            version = upstreamVersion + "~" + distribution + "1"
        print >> f, "nextcloud-client (%s) %s; urgency=medium" % (version, distribution)
        print >> f
        print >> f, "  * " + subject
        print >> f
        print >> f, " -- %s <%s>  %s" % (name, email, date)
        print >> f
    return (latestBaseVersion, latestKind)

if __name__ == "__main__":

    distribution = sys.argv[2]

    #entries = collectEntries("8aade24147b5313f8241a8b42331442b7f40eef9", "2.2.4", "release")
    entries = collectEntries("f9b1c724d6ab5431e0cd56b7cd834f2dd48cebb1", "2.4.0", "beta")


    with open(sys.argv[1], "wt") as f:
        (baseVersion, kind) = genChangeLogEntries(f, entries, distribution)
        print baseVersion, kind
