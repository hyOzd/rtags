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

#ifndef GCCARGUMENTS_H
#define GCCARGUMENTS_H

#include <rct/Path.h>
#include <rct/List.h>
#include <rct/Hash.h>
#include <rct/String.h>
#include <rct/Serializer.h>

class SourceInformation;
class GccArguments
{
public:
    enum Language {
        NoLanguage,
        C,
        CPlusPlus,
        CPlusPlus11,
        CHeader,
        CPlusPlusHeader,
        CPlusPlus11Header
    };

    static Language guessLanguageFromCompiler(const Path &compiler);
    static Language guessLanguageFromSourceFile(const Path &sourceFile);

    GccArguments();

    bool parse(String args, const Path &base);
    bool parse(List<String> split, const Path &base);
    void init(const SourceInformation &sourceInformation);

    Language language() const { return mLanguage; }
    bool isValid() const { return mLanguage != NoLanguage; }
    void clear();

    void addFlags(const List<String> &extraFlags);
    const List<String> &clangArgs() const { return mClangArgs; }
    const List<Path> &inputFiles() const { return mInputFiles; }
    const List<Path> &includePaths() const { return mIncludePaths; }
    const List<Path> &unresolvedInputFiles() const { return mUnresolvedInputFiles; }
    const Path &baseDirectory() const { return mBase; }
    const Path &compiler() const { return mCompiler; }
    Path projectRoot() const;
    struct Define {
        String define;
        String value;
    };
    const List<Define> &defines() const { return mDefines; }
    void encode(Serializer &serializer) const;
    void decode(Deserializer &deserializer);
private:
    List<String> mClangArgs;
    List<Define> mDefines;
    List<Path> mInputFiles, mUnresolvedInputFiles, mIncludePaths;
    Path mBase, mCompiler;
    GccArguments::Language mLanguage;
    friend class MakefileParser;
    friend class Server;
};

template <> inline Serializer &operator<<(Serializer &s, const GccArguments &a)
{
    a.encode(s);
    return s;
}

template <> inline Deserializer &operator>>(Deserializer &s, GccArguments &a)
{
    a.decode(s);
    return s;
}

#endif
