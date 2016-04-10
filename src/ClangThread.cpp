/* This file is part of RTags (http://rtags.net).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "ClangThread.h"
#include "Symbol.h"

#include "rct/Connection.h"
#include "RTags.h"
#include "Server.h"

struct Dep : public DependencyNode
{
    Dep(uint32_t f)
        : DependencyNode(f)
    {}
    Hash<uint32_t, Map<Location, Location> > references;
};

ClangThread::ClangThread(const std::shared_ptr<QueryMessage> &queryMessage, const Source &source, const std::shared_ptr<Connection> &conn)
    : Thread(), mQueryMessage(queryMessage), mSource(source), mConnection(conn), mIndentLevel(0), mAborted(false)
{
    setAutoDelete(true);
}

static const CXSourceLocation nullLocation = clang_getNullLocation();
static const CXCursor nullCursor = clang_getNullCursor();

CXChildVisitResult ClangThread::visitor(CXCursor cursor, CXCursor, CXClientData userData)
{
    ClangThread *that = reinterpret_cast<ClangThread*>(userData);
    assert(that);
    return that->visit(cursor);
}

CXChildVisitResult ClangThread::visit(const CXCursor &cursor)
{
    if (isAborted())
        return CXChildVisit_Break;
    const Location location = createLocation(cursor);
    if (!location.isNull()) {
        if (mQueryMessage->flags() & QueryMessage::DumpCheckIncludes) {
            checkIncludes(location, cursor);
            return CXChildVisit_Recurse;
        } else if (mQueryMessage->type() == QueryMessage::VisitAST) {
            addCursor(cursor, location);
            return CXChildVisit_Recurse;
        } else {
            Flags<Location::ToStringFlag> locationFlags;
            if (mQueryMessage->flags() & QueryMessage::NoColor)
                locationFlags |= Location::NoColor;

            CXSourceRange range = clang_getCursorExtent(cursor);
            CXSourceLocation rangeEnd = clang_getRangeEnd(range);
            unsigned int endLine, endColumn;
            clang_getPresumedLocation(rangeEnd, 0, &endLine, &endColumn);
            if (!(mQueryMessage->flags() & QueryMessage::DumpIncludeHeaders) && location.fileId() != mSource.fileId) {
                return CXChildVisit_Continue;
            }

            String message;
            message.reserve(256);

            if (!(mQueryMessage->flags() & QueryMessage::NoContext)) {
                message = location.context(locationFlags, &mContextCache);
            }

            if (endLine == location.line()) {
                message += String::format<32>(" // %d-%d, %d: ", location.column(), endColumn, mIndentLevel);
            } else {
                message += String::format<32>(" // %d-%d:%d, %d: ", location.column(), endLine, endColumn, mIndentLevel);
            }
            message += RTags::cursorToString(cursor, RTags::AllCursorToStringFlags);
            message.append(" " + RTags::typeName(cursor));;
            if (clang_getCursorKind(cursor) == CXCursor_VarDecl) {
                const std::shared_ptr<RTags::Auto> autoResolved = RTags::resolveAuto(cursor);
                if (autoResolved && !clang_equalCursors(autoResolved->cursor, nullCursor)) {
                    message += "auto resolves to " + RTags::cursorToString(autoResolved->cursor, RTags::AllCursorToStringFlags);
                }
            }
            CXCursor ref = clang_getCursorReferenced(cursor);
            if (clang_equalCursors(ref, cursor)) {
                message.append("refs self");
            } else if (!clang_equalCursors(ref, nullCursor)) {
                message.append("refs ");
                message.append(RTags::cursorToString(ref, RTags::AllCursorToStringFlags));
            }

            CXCursor canonical = clang_getCanonicalCursor(cursor);
            if (!clang_equalCursors(canonical, cursor) && !clang_equalCursors(canonical, nullCursor)) {
                message.append("canonical ");
                message.append(RTags::cursorToString(canonical, RTags::AllCursorToStringFlags));
            }

            CXCursor specialized = clang_getSpecializedCursorTemplate(cursor);
            if (!clang_equalCursors(specialized, cursor) && !clang_equalCursors(specialized, nullCursor)) {
                message.append("specialized ");
                message.append(RTags::cursorToString(specialized, RTags::AllCursorToStringFlags));
            }

            writeToConnetion(message);
        }
    }
    ++mIndentLevel;
    clang_visitChildren(cursor, ClangThread::visitor, this);
    if (isAborted())
        return CXChildVisit_Break;
    --mIndentLevel;
    return CXChildVisit_Continue;
}

void ClangThread::run()
{
    const auto key = mConnection->disconnected().connect([this](const std::shared_ptr<Connection> &) { abort(); });

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit translationUnit = 0;
    String clangLine;
    RTags::parseTranslationUnit(mSource.sourceFile(), mSource.toCommandLine(Source::Default), translationUnit,
                                index, 0, 0, CXTranslationUnit_DetailedPreprocessingRecord, &clangLine);
    if (!(mQueryMessage->flags() & QueryMessage::DumpCheckIncludes) && mQueryMessage->type() != QueryMessage::VisitAST)
        writeToConnetion(String::format<128>("Indexed: %s => %s", clangLine.constData(), translationUnit ? "success" : "failure"));
    if (translationUnit) {
        clang_visitChildren(clang_getTranslationUnitCursor(translationUnit), ClangThread::visitor, this);
    } else if (mQueryMessage->type() == QueryMessage::VisitAST) {
        writeToConnetion(String::format<1024>("{ \"file\": \"%s\", \"commandLine\": \"%s\", \"success\": false }",
                                              mSource.sourceFile().constData(),
                                              String::join(mSource.toCommandLine(Source::Default), ' ').constData()));
    }

    mConnection->disconnected().disconnect(key);
    if (mQueryMessage->flags() & QueryMessage::DumpCheckIncludes) {
        checkIncludes();
    }
    if (translationUnit)
        clang_disposeTranslationUnit(translationUnit);

    clang_disposeIndex(index);

    std::weak_ptr<Connection> conn = mConnection;
    EventLoop::mainEventLoop()->callLater([conn]() {
            if (auto c = conn.lock())
                c->finish();
        });
}

void ClangThread::writeToConnetion(const String &message)
{
    std::weak_ptr<Connection> conn = mConnection;
    EventLoop::mainEventLoop()->callLater([conn, message]() {
            if (auto c = conn.lock()) {
                c->write(message);
            }
        });
}

void ClangThread::handleInclude(Location loc, const CXCursor &cursor)
{
    CXFile includedFile = clang_getIncludedFile(cursor);
    if (includedFile) {
        CXStringScope fn = clang_getFileName(includedFile);
        const char *cstr = clang_getCString(fn);
        if (!cstr) {
            clang_disposeString(fn);
            return;
        }
        const Path p = Path::resolved(cstr);
        clang_disposeString(fn);
        const uint32_t fileId = Location::insertFile(p);
        Dep *&source = mDependencies[loc.fileId()];
        if (!source)
            source = new Dep(loc.fileId());
        Dep *&include = mDependencies[fileId];
        if (!include)
            include = new Dep(fileId);
        source->include(include);
    }
}

void ClangThread::handleReference(Location loc, const CXCursor &ref)
{
    if (clang_getCursorKind(ref) == CXCursor_Namespace)
        return;
    const Location refLoc = createLocation(ref);
    if (refLoc.isNull() || refLoc.fileId() == loc.fileId())
        return;

    Dep *dep = mDependencies[loc.fileId()];
    assert(dep);
    Dep *refDep = mDependencies[refLoc.fileId()];
    assert(refDep);
    auto &refs = dep->references[refDep->fileId];
    refs[loc] = refLoc;
}

void ClangThread::checkIncludes(Location location, const CXCursor &cursor)
{
    if (clang_getCursorKind(cursor) == CXCursor_InclusionDirective) {
        handleInclude(location, cursor);
    } else {
        const CXCursor ref = clang_getCursorReferenced(cursor);
        if (!clang_equalCursors(cursor, nullCursor) && !clang_equalCursors(cursor, ref)) {
            handleReference(location, ref);
        }
    }
}

static bool validateHasInclude(uint32_t ref, const Dep *cur, Set<uint32_t> &seen)
{
    assert(ref);
    assert(cur);
    if (cur->includes.contains(ref))
        return true;
    if (!seen.insert(ref))
        return false;
    for (const auto &pair : cur->includes) {
        if (validateHasInclude(ref, static_cast<const Dep*>(pair.second), seen))
            return true;
    }
    return false;
}

static bool validateNeedsInclude(const Dep *source, const Dep *header, Set<uint32_t> &seen)
{
    if (!seen.insert(header->fileId)) {
        // error() << "already seen" << Location::path(source->fileId);
        return false;
    }
    if (source->references.contains(header->fileId)) {
        // error() << "Got ref" << Location::path(header->fileId);
        return true;
    }
    for (const auto &child : header->includes) {
        // error() << "Checking child" << Location::path(child.second->fileId);
        if (validateNeedsInclude(source, static_cast<const Dep*>(child.second), seen)) {
            return true;
        }
    }

    // error() << "Checking" << Location::path(source->fileId) << "doesn't seem to need" << Location::path(header->fileId) << depth;
    return false;
}

void ClangThread::checkIncludes()
{
    for (const auto &it : mDependencies) {
        const Path path = Location::path(it.first);
        if (path.isSystem())
            continue;

        for (const auto &dep  : it.second->includes) {
            Set<uint32_t> seen;
            if (!validateNeedsInclude(it.second, static_cast<Dep*>(dep.second), seen)) {
                writeToConnetion(String::format<128>("%s includes %s for no reason",
                                                     path.constData(),
                                                     Location::path(dep.second->fileId).constData()));
            }
        }

        for (const auto &ref : it.second->references) {
            const Path refPath = Location::path(ref.first);
            if (refPath.startsWith("/usr/include/sys/_types/_") || refPath.startsWith("/usr/include/_types/_"))
                continue;
            Set<uint32_t> seen;
            if (!validateHasInclude(ref.first, it.second, seen)) {
                List<String> reasons;
                for (const auto &r : ref.second) {
                    String reason;
                    Log log(&reason);
                    log << r.first << "=>" << r.second;
                    reasons << reason;
                }
                writeToConnetion(String::format<128>("%s should include %s (%s)",
                                                     Location::path(it.first).constData(),
                                                     Location::path(ref.first).constData(),
                                                     String::join(reasons, " ").constData()));
                // for (const auto &incs : mDependencies[ref.first]->dependents) {
                //     writeToConnetion(String::format<128>("GOT INCLUDER %s:%d", Location::path(incs.first).constData(),
                //                                          incs.first));
                // }
            }
        }
    }

    for (auto it : mDependencies) {
        delete it.second;
    }
}

ClangThread::Cursor *ClangThread::addCursor(CXCursor cursor, Location location)
{
    if (location.isNull()) {
        location = createLocation(cursor);
        if (location.isNull())
            return 0;
    }

    const String usr = RTags::eatString(clang_getCursorUSR(cursor));
    if (!usr.isEmpty()) {
        Cursor *ret = mCursorsByUsr.value(usr);
        if (ret)
            return ret;
    }

    std::shared_ptr<Cursor> c(new Cursor);
    c->usr = usr;
    c->location = location;
    const CXSourceRange range = clang_getCursorExtent(cursor);
    if (!clang_Range_isNull(range)) {
        c->rangeStart = createLocation(clang_getRangeStart(range));
        c->rangeEnd = createLocation(clang_getRangeEnd(range));
    }
    const CXCursor ref = clang_getCursorReferenced(cursor);
    if (!clang_equalCursors(ref, nullCursor)) {
        c->referenced = addCursor(ref);
    }

    if (!c->usr.isEmpty()) {
        mCursorsByUsr[c->usr] = c.get();
    }

    c->kind = RTags::eatString(clang_getCursorKindSpelling(clang_getCursorKind(cursor)));
    c->linkage = RTags::linkageSpelling(clang_getCursorLinkage(cursor));
    c->spelling = RTags::eatString(clang_getCursorSpelling(cursor));
    c->displayName = RTags::eatString(clang_getCursorDisplayName(cursor));
    c->mangledName = RTags::eatString(clang_Cursor_getMangling(cursor));
    const CXCursorKind templateKind = clang_getTemplateCursorKind(cursor);
    if (templateKind != CXCursor_NoDeclFound) {
        c->templateKind = RTags::eatString(clang_getCursorKindSpelling(templateKind));
    }
    c->lexicalParent = addCursor(clang_getCursorLexicalParent(cursor));
    c->semanticParent = addCursor(clang_getCursorSemanticParent(cursor));
    c->lexicalParent = addCursor(clang_getCursorSemanticParent(cursor));
    c->canonical = addCursor(clang_getCanonicalCursor(cursor));
    if (clang_isCursorDefinition(cursor)) {
        c->flags |= Cursor::Definition;
    } else {
        c->definition = addCursor(clang_getCursorDefinition(cursor));
    }
    c->specializedCursorTemplate = addCursor(clang_getSpecializedCursorTemplate(cursor));

    {
        CXCursor *overridden;
        unsigned int count;
        clang_getOverriddenCursors(cursor, &overridden, &count);
        if (overridden) {
            for (unsigned i=0; i<count; ++i) {
                if (Cursor *o = addCursor(overridden[i]))
                    c->overridden.append(o);
            }
            clang_disposeOverriddenCursors(overridden);
        }
    }
    {

    }

    // CINDEX_LINKAGE CXFile clang_getIncludedFile(CXCursor cursor);

    c->type = addType(clang_getCursorType(cursor));
    // c->type = addType(clang_getCursorType(cursor));

    return 0;
}

ClangThread::Type *ClangThread::addType(CXType /*type*/)
{
    return 0;
}
