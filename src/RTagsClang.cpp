/* This file is part of RTags.

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

#include "RTagsClang.h"
#include "Server.h"
#include "GccArguments.h"
#include "CompilerManager.h"

#include <iostream>

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#ifdef HAVE_BACKTRACE
#undef HAVE_BACKTRACE
#endif
#include <llvm/Config/config.h>

#include <clang/Driver/ArgList.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/ToolChain.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Host.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
// #include <clang/Basic/Diagnostic.h>
// #include <clang/Basic/FileManager.h>
// #include <clang/Basic/SourceManager.h>
#include <clang/Basic/TargetInfo.h>
// #include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/HeaderSearchOptions.h>

namespace RTags {
String eatString(CXString str)
{
    const String ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

String cursorToString(CXCursor cursor, unsigned flags)
{
    const CXCursorKind kind = clang_getCursorKind(cursor);
    String ret;
    ret.reserve(256);
    ret += eatString(clang_getCursorKindSpelling(kind));
    if (clang_isInvalid(kind))
        return ret;

    switch (RTags::cursorType(kind)) {
    case Reference:
        ret += " r";
        break;
    case Cursor:
        ret += " c";
        break;
    case Other:
        ret += " o";
        break;
    case Include:
        ret += " i";
        break;
    }

    const String name = eatString(clang_getCursorDisplayName(cursor));
    const String other = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty())
        ret += " " + name;
    if (other != name && !other.isEmpty())
        ret += " " + other;

    if (clang_isCursorDefinition(cursor))
        ret += " def";

    if (flags & IncludeUSR)
        ret += " " + eatString(clang_getCursorUSR(cursor));

    CXFile file;
    unsigned off, line, col; //presumedLine, presumedCol, instantiationLoc, expansionLoc;
    CXSourceLocation location = clang_getCursorLocation(cursor);
    clang_getSpellingLocation(location, &file, &line, &col, &off);
    // clang_getPresumedLocation(location, 0, &presumedLine, &presumedCol);
    // clang_getInstantiationLocation(location, 0, 0, 0, &instantiationLoc);
    // clang_getExpansionLocation(location, 0, 0, 0, &expansionLoc);

    const Str fileName(clang_getFileName(file));
    if (fileName.data() && *fileName.data()) {
        ret += ' ';
        ret += fileName.data();
        ret += ',';
        ret += String::number(off);

        if (flags & IncludeRange) {
            ret += " (";
            CXSourceRange range = clang_getCursorExtent(cursor);
            unsigned start, end;
            clang_getSpellingLocation(clang_getRangeStart(range), 0, 0, 0, &start);
            clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &end);
            ret += String::number(start);
            ret += '-';
            ret += String::number(end);
            ret += ')';
        }

        // if (presumedLine != line || presumedCol != col)
        //     ret += String::snprintf<32>("presumed: %d:%d", presumedLine, presumedCol);
        // if (instantiationLoc != off)
        //     ret += String::snprintf<32>("instantiation: %d", instantiationLoc);
        // if (expansionLoc != off)
        //     ret += String::snprintf<32>("expansion: %d", expansionLoc);

    }
    return ret;
}

static inline SymbolMap::const_iterator findCursorInfo(const SymbolMap &map, const Location &location, const String &context, bool scan)
{
    if (context.isEmpty() || !scan) {
        SymbolMap::const_iterator it = map.lower_bound(location);
        if (it != map.end() && it->first == location) {
            return it;
        } else if (it != map.begin()) {
            --it;
            if (it->first.fileId() == location.fileId()) {
                const int off = location.offset() - it->first.offset();
                if (it->second.symbolLength > off && (context.isEmpty() || it->second.symbolName.contains(context))) {
                    return it;
                }
            }
        }
        return map.end();
    }

    SymbolMap::const_iterator f = map.lower_bound(location);
    if (f != map.begin() && (f == map.end() || f->first != location))
        --f;
    SymbolMap::const_iterator b = f;

    enum { Search = 32 };
    for (int j=0; j<Search; ++j) {
        if (f != map.end()) {
            if (location.fileId() != f->first.fileId()) {
                if (b == map.begin())
                    break;
                f = map.end();
            } else if (f->second.symbolName.contains(context)) {
                // error() << "found it forward" << j;
                return f;
            } else {
                ++f;
            }
        }

        if (b != map.begin()) {
            --b;
            if (location.fileId() != b->first.fileId()) {
                if (f == map.end())
                    break;
                b = map.begin();
            } else if (b->second.symbolName.contains(context)) {
                // error() << "found it backward" << j;
                return b;
            }
        }
    }
    return map.end();
}

SymbolMap::const_iterator findCursorInfo(const SymbolMap &map, const Location &location, const String &context,
                                         const SymbolMap *errors, bool *foundInErrors)
{
    if (foundInErrors)
        *foundInErrors = false;
    if (map.isEmpty() && !errors)
        return map.end();
    if (errors) {
        SymbolMap::const_iterator ret = findCursorInfo(map, location, context, false);
        if (ret != map.end()) {
            return ret;
        }
        ret = findCursorInfo(*errors, location, context, false);
        if (ret != errors->end()) {
            if (foundInErrors)
                *foundInErrors = true;
            return ret;
        }
        // ret = findCursorInfo(*errors, location, context, true);
        // if (ret != errors->end()) {
        //     if (foundInErrors)
        //         *foundInErrors = true;
        //     return ret;
        // }
        // ret = findCursorInfo(map, location, context, true);
        // if (ret != map.end()) {
        //     return ret;
        // }

        return map.end();
    } else {
        const SymbolMap::const_iterator ret = findCursorInfo(map, location, context, true);
        return ret;
    }
}

void parseTranslationUnit(const Path &sourceFile, const List<String> &args,
                          const List<String> &defaultArguments,
                          CXTranslationUnit &unit, CXIndex index, String &clangLine,
                          CXUnsavedFile *unsaved, int unsavedCount)

{
    clangLine = "clang ";

    int idx = 0;
    List<const char*> clangArgs(args.size() + defaultArguments.size(), 0);

    const List<String> *lists[] = { &args, &defaultArguments };
    for (int i=0; i<2; ++i) {
        const int count = lists[i]->size();
        for (int j=0; j<count; ++j) {
            String arg = lists[i]->at(j);
            if (arg.isEmpty())
                continue;

            clangArgs[idx++] = lists[i]->at(j).constData();
            arg.replace("\"", "\\\"");
            clangLine += arg;
            clangLine += ' ';
        }
    }

    clangLine += sourceFile;

    StopWatch sw;
    unsigned int flags = CXTranslationUnit_DetailedPreprocessingRecord;
    if (Server::instance() && Server::instance()->options().completionCacheSize)
        flags |= CXTranslationUnit_PrecompiledPreamble|CXTranslationUnit_CacheCompletionResults;

    unit = clang_parseTranslationUnit(index, sourceFile.constData(),
                                      clangArgs.data(), idx, unsaved, unsavedCount, flags);
    // error() << sourceFile << sw.elapsed();
}

void reparseTranslationUnit(CXTranslationUnit &unit, CXUnsavedFile *unsaved, int unsavedCount)
{
    assert(unit);
    if (clang_reparseTranslationUnit(unit, 0, unsaved, clang_defaultReparseOptions(unit)) != 0) {
        clang_disposeTranslationUnit(unit);
        unit = 0;
    }
}

class StringOStream : public llvm::raw_ostream
{
public:
    StringOStream()
        : llvm::raw_ostream(true) // non-buffered
    {}
    virtual void write_impl(const char *data, size_t size)
    {
        mString.append(data, size);
    }
    virtual uint64_t current_pos() const
    {
        return mString.size();
    }
    String &&take() { return std::move(mString); }
private:
    String mString;
};

String preprocess(const GccArguments &args)
{
    const List<Path> inputs = args.inputFiles();
    if (!inputs.isEmpty()) {
        return preprocess(inputs.first(), args.compiler(), args.language(), args.includePaths(), args.defines());
    }
    return String();
}

static inline void processArgs(clang::HeaderSearchOptions &headerSearchOptions,
                               const clang::driver::ArgStringList &args)
{
    enum Type {
        Pending,
        SystemInclude
    } type = Pending;
    for (const char *arg : args) {
        switch (type) {
        case Pending:
            if (!strcmp(arg, "-internal-isystem") || !strcmp(arg, "-internal-externc-isystem")) {
                type = SystemInclude;
            } else if (!strncmp("-l", arg, 2)) {

            } else {
                error() << "Unknown arg type" << arg;
            }
            break;
        case SystemInclude: {
            const Path path = Path::resolved(arg);
            error() << "Adding system include" << path;
            headerSearchOptions.AddPath(clang::StringRef(path.constData(), path.size()), clang::frontend::System, false, false);
            type = Pending;
            break; }
        }
        // printf("got clang cxx arg %s\n", arg);
    }
}

String preprocess(const Path &sourceFile, const Path &compiler, GccArguments::Language language,
                  const List<Path> &includePaths, const List<GccArguments::Define> &defines)
{
    clang::CompilerInstance compilerInstance;
    compilerInstance.createFileManager();
    assert(compilerInstance.hasFileManager());
    compilerInstance.createDiagnostics();
    assert(compilerInstance.hasDiagnostics());

    clang::FileManager &fm = compilerInstance.getFileManager();
    compilerInstance.createSourceManager(fm);
    assert(compilerInstance.hasSourceManager());
    clang::SourceManager &sm = compilerInstance.getSourceManager();
    const clang::FileEntry *file = fm.getFile(sourceFile.constData(), true); // pass openfile?
    if (!file)
        return String();
    sm.createMainFileID(file);
    clang::TargetOptions &targetOptions = compilerInstance.getTargetOpts();
    //targetOptions.Triple = LLVM_HOST_TRIPLE;
    targetOptions.Triple = llvm::sys::getDefaultTargetTriple();
    clang::DiagnosticsEngine& diags = compilerInstance.getDiagnostics();
    compilerInstance.setTarget(clang::TargetInfo::CreateTargetInfo(diags, &targetOptions));
    clang::LangOptions &langOpts = compilerInstance.getLangOpts();
    switch (language) {
    case GccArguments::CPlusPlus11:
        langOpts.CPlusPlus11 = true;
        langOpts.CPlusPlus = true;
        break;
    case GccArguments::CPlusPlus:
        langOpts.CPlusPlus = true;
        break;
    default:
        break;
    }
    clang::HeaderSearchOptions &headerSearchOptions = compilerInstance.getHeaderSearchOpts();
    {
        clang::driver::Driver driver("clang", llvm::sys::getDefaultTargetTriple(), "a.out", diags);
        std::vector<std::string> copies; // not cool
        std::vector<const char*> args;
        args.push_back(compiler.constData());
        args.push_back("-c");
        args.push_back(sourceFile.constData());
        for (const Path& path : includePaths) {
            copies.push_back("-I" + path);
        }
        for (const GccArguments::Define& def : defines) {
            copies.push_back("-D" + def.define);
            if (!def.value.isEmpty())
                copies.back() += "=" + def.value;
        }
        for (const std::string& str : copies) {
            args.push_back(str.c_str());
        }

        std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(llvm::ArrayRef<const char*>(&args[0], args.size())));
        const clang::driver::ToolChain& toolChain = compilation->getDefaultToolChain();
        const clang::driver::InputArgList inputArgs(&args[0], &args[0] + args.size());
        clang::driver::ArgStringList outputArgs;
        toolChain.AddClangCXXStdlibIncludeArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
        toolChain.AddClangSystemIncludeArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
        toolChain.AddCXXStdlibLibArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
    }

    for (List<Path>::const_iterator it = includePaths.begin(); it != includePaths.end(); ++it) {
        error() << "Adding -I" << *it;
        headerSearchOptions.AddPath(clang::StringRef(it->constData(), it->size()),
                                    clang::frontend::Angled, false, false);
    }

    // List<Path> systemIncludes;
    // List<GccArguments::Define> systemDefines;
    // CompilerManager::data(compiler, systemDefines, systemIncludes);
    // for (List<Path>::const_iterator it = systemIncludes.begin(); it != systemIncludes.end(); ++it) {
    //     headerSearchOptions.AddPath(clang::StringRef(it->constData(), it->size()),
    //                                 clang::frontend::System, false, false);
    //     error() << "Adding system path" << *it;
    // }

    // for (List<GccArguments::Define>::const_iterator it = systemDefines.begin(); it != systemDefines.end(); ++it) {
    //     predefines += "#define ";
    //     predefines += it->define;
    //     if (!it->value.isEmpty()) {
    //         predefines += ' ';
    //         predefines += it->value;
    //     }
    //     predefines += '\n';
    //     // error() << "Got define" << it->define << it->value;
    // }

    compilerInstance.createPreprocessor();
    std::string predefines = compilerInstance.getPreprocessor().getPredefines();
    for (List<GccArguments::Define>::const_iterator it = defines.begin(); it != defines.end(); ++it) {
        predefines += "#define ";
        predefines += it->define;
        if (!it->value.isEmpty()) {
            predefines += ' ';
            predefines += it->value;
        }
        predefines += '\n';
        // error() << "Got define" << it->define << it->value;
    }


    // error() << "predefines" << compilerInstance.getPreprocessor().getPredefines();
    compilerInstance.getPreprocessor().setPredefines(predefines);
    StringOStream out;
    clang::PreprocessorOutputOptions preprocessorOptions;
    preprocessorOptions.ShowCPP = 1;

    compilerInstance.getDiagnosticClient().BeginSourceFile(compilerInstance.getLangOpts(), &compilerInstance.getPreprocessor());

    clang::DoPrintPreprocessedInput(compilerInstance.getPreprocessor(), &out, preprocessorOptions);
    FILE *f = fopen("/tmp/preprocess.cpp", "w");
    // fwrite(sourceFile.constData(), 1, sourceFile.size(), f);

    const String str = out.take();
    fwrite(str.constData(), 1, str.size(), f);
    return str;
    // return out.take();
}

static CXChildVisitResult findFirstChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    *reinterpret_cast<CXCursor*>(data) = cursor;
    return CXChildVisit_Break;
}

CXCursor findFirstChild(CXCursor parent)
{
    CXCursor ret = clang_getNullCursor();
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findFirstChildVisitor, &ret);
    return ret;
}

struct FindChildVisitor
{
    CXCursorKind kind;
    String name;
    CXCursor cursor;
};

static CXChildVisitResult findChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChildVisitor *u = reinterpret_cast<FindChildVisitor*>(data);
    if (u->name.isEmpty()) {
        if (clang_getCursorKind(cursor) == u->kind) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    } else {
        CXStringScope str = clang_getCursorSpelling(cursor);
        if (str.data() && u->name == str.data()) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    }
    return CXChildVisit_Continue;
}

CXCursor findChild(CXCursor parent, CXCursorKind kind)
{
    FindChildVisitor u = { kind, String(), clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

CXCursor findChild(CXCursor parent, const String &name)
{
    FindChildVisitor u = { CXCursor_FirstInvalid, name, clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

struct ChildrenVisitor
{
    const Filter &in;
    const Filter &out;
    List<CXCursor> children;
};

static CXChildVisitResult childrenVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    ChildrenVisitor *u = reinterpret_cast<ChildrenVisitor*>(data);
    if ((u->out.isNull() || !u->out.match(cursor)) && (u->in.isNull() || u->in.match(cursor))) {
        u->children.append(cursor);
    }
    return CXChildVisit_Continue;
}

List<CXCursor> children(CXCursor parent, const Filter &in, const Filter &out)
{
    ChildrenVisitor userData = { in, out, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, childrenVisitor, &userData);
    return userData.children;
}

struct FindChainVisitor
{
    const List<CXCursorKind> &kinds;
    List<CXCursor> ret;
};

static CXChildVisitResult findChainVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChainVisitor *u = reinterpret_cast<FindChainVisitor*>(data);
    if (clang_getCursorKind(cursor) == u->kinds.at(u->ret.size())) {
        u->ret.append(cursor);
        if (u->ret.size() < u->kinds.size())
            return CXChildVisit_Recurse;

        return CXChildVisit_Break;
    }
    return CXChildVisit_Break;
}

List<CXCursor> findChain(CXCursor parent, const List<CXCursorKind> &kinds)
{
    assert(!kinds.isEmpty());
    FindChainVisitor userData = { kinds, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChainVisitor, &userData);
    if (userData.ret.size() != kinds.size()) {
        userData.ret.clear();
    }
    return userData.ret;
}
}
