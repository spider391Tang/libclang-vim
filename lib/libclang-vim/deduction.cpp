#include "deduction.hpp"

#include <clang-c/CXCompilationDatabase.h>

namespace {

/// Look up compilation arguments for a file from a database in one of its
/// parent directories.
libclang_vim::args_type parse_compilation_database(const std::string& file) {
    libclang_vim::args_type ret;

    std::size_t found = file.find_last_of("/\\");
    std::string directory = file.substr(0, found);
    while (true) {
        std::string json =
            directory + file.substr(found, 1) + "compile_commands.json";
        std::ifstream stream(json.c_str());
        if (stream.good())
            break;

        found = directory.find_last_of("/\\");
        if (found == std::string::npos)
            break;

        directory = directory.substr(0, found);
    }

    if (directory.empty()) {
        // Our default when no JSON was found.
        ret.emplace_back("-std=c++1y");

        return ret;
    }

    CXCompilationDatabase_Error error;
    CXCompilationDatabase database =
        clang_CompilationDatabase_fromDirectory(directory.c_str(), &error);
    if (error == CXCompilationDatabase_NoError) {
        CXCompileCommands commands =
            clang_CompilationDatabase_getCompileCommands(database,
                                                         file.c_str());
        unsigned commandsSize = clang_CompileCommands_getSize(commands);
        if (commandsSize >= 1) {
            CXCompileCommand command =
                clang_CompileCommands_getCommand(commands, 0);
            unsigned args = clang_CompileCommand_getNumArgs(command);
            for (unsigned i = 0; i < args; ++i) {
                libclang_vim::cxstring_ptr arg =
                    clang_CompileCommand_getArg(command, i);
                if (file != clang_getCString(arg))
                    ret.emplace_back(clang_getCString(arg));
            }
        }
        clang_CompileCommands_dispose(commands);
    }
    clang_CompilationDatabase_dispose(database);

    return ret;
}

CXChildVisitResult valid_type_cursor_getter(CXCursor cursor, CXCursor,
                                            CXClientData data) {
    auto const type = clang_getCursorType(cursor);
    if (type.kind != CXType_Invalid) {
        *(reinterpret_cast<CXCursor*>(data)) = cursor;
        return CXChildVisit_Break;
    }
    return CXChildVisit_Recurse;
}

bool is_invalid_type_cursor(CXCursor const& cursor) {
    return clang_getCursorType(cursor).kind == CXType_Invalid;
}

bool is_auto_type(const std::string& type_name) {
    for (auto pos = type_name.find("auto"); pos != std::string::npos;
         pos = type_name.find("auto", pos + 1)) {

        if (pos != 0) {
            if (std::isalnum(type_name[pos - 1]) || type_name[pos - 1] == '_') {
                continue;
            }
        }

        if (pos + 3 /*pos of 'o'*/ < type_name.size() - 1) {
            if (std::isalnum(type_name[pos + 3 + 1]) ||
                type_name[pos + 3 + 1] == '_') {
                continue;
            }
        }

        return true;
    }

    return false;
}

CXChildVisitResult unexposed_type_deducer(CXCursor cursor, CXCursor,
                                          CXClientData data) {
    const auto type = clang_getCursorType(cursor);
    libclang_vim::cxstring_ptr type_name = clang_getTypeSpelling(type);
    if (type.kind == CXType_Invalid || is_auto_type(to_c_str(type_name))) {
        clang_visitChildren(cursor, unexposed_type_deducer, data);
        return CXChildVisit_Continue;
    } else {
        *(reinterpret_cast<CXType*>(data)) = type;
        return CXChildVisit_Break;
    }
}

CXType deduce_func_decl_type_at_cursor(CXCursor const& cursor) {
    auto const func_type = clang_getCursorType(cursor);
    auto const result_type = clang_getResultType(func_type);

    switch (result_type.kind) {
    case CXType_Unexposed: {
        libclang_vim::cxstring_ptr type_name =
            clang_getTypeSpelling(result_type);
        if (std::strcmp(to_c_str(type_name), "auto") != 0) {
            return result_type;
        }
    }
    case CXType_Invalid: {
        // When (unexposed and "auto") or invalid

        // Get cursor at a return statement
        CXCursor const return_stmt_cursor =
            libclang_vim::search_kind(cursor, [](CXCursorKind const& kind) {
                return kind == CXCursor_ReturnStmt;
            });
        if (clang_Cursor_isNull(return_stmt_cursor)) {
            return clang_getCursorType(return_stmt_cursor);
        }

        CXType deduced_type;
        deduced_type.kind = CXType_Invalid;
        clang_visitChildren(return_stmt_cursor, unexposed_type_deducer,
                            &deduced_type);
        return deduced_type;
    }
    default:
        return result_type;
    }
}

CXType deduce_type_at_cursor(const CXCursor& cursor) {
    auto const type = clang_getCursorType(cursor);
    libclang_vim::cxstring_ptr type_name = clang_getTypeSpelling(type);
    if (type.kind == CXType_Invalid || is_auto_type(to_c_str(type_name))) {
        CXType deduced_type;
        deduced_type.kind = CXType_Invalid;
        clang_visitChildren(cursor, unexposed_type_deducer, &deduced_type);
        return deduced_type;
    } else {
        return type;
    }
}
}

const char*
libclang_vim::deduce_var_decl_type(const location_tuple& location_info) {
    return at_specific_location(
        location_info, [](const CXCursor& cursor) -> std::string {
            const CXCursor var_decl_cursor =
                search_kind(cursor, [](const CXCursorKind& kind) {
                    return kind == CXCursor_VarDecl;
                });
            if (clang_Cursor_isNull(var_decl_cursor)) {
                return "{}";
            }

            const CXType var_type = deduce_type_at_cursor(var_decl_cursor);
            if (var_type.kind == CXType_Invalid) {
                return "{}";
            }

            std::string result;
            result += stringize_type(var_type);
            result += "'canonical':{" +
                      stringize_type(clang_getCanonicalType(var_type)) + "},";
            return "{" + result + "}";
        });
}

const char*
libclang_vim::deduce_func_or_var_decl(const location_tuple& location_info) {
    return at_specific_location(
        location_info, [](const CXCursor& cursor) -> std::string {
            const CXCursor func_or_var_decl =
                search_kind(cursor, [](const CXCursorKind& kind) {
                    return kind == CXCursor_VarDecl ||
                           is_function_decl_kind(kind);
                });
            if (clang_Cursor_isNull(func_or_var_decl)) {
                return "{}";
            }

            const CXType result_type =
                clang_getCursorKind(cursor) == CXCursor_VarDecl
                    ? deduce_type_at_cursor(func_or_var_decl)
                    : deduce_func_decl_type_at_cursor(func_or_var_decl);
            if (result_type.kind == CXType_Invalid) {
                return "{}";
            }

            std::string result;
            result += stringize_type(result_type);
            result += "'canonical':{" +
                      stringize_type(clang_getCanonicalType(result_type)) +
                      "},";
            return "{" + result + "}";
        });
}

const char*
libclang_vim::deduce_func_return_type(const location_tuple& location_info) {
    return at_specific_location(
        location_info, [](CXCursor const& cursor) -> std::string {
            CXCursor const func_decl_cursor =
                search_kind(cursor, [](const CXCursorKind& kind) {
                    return is_function_decl_kind(kind);
                });
            if (clang_Cursor_isNull(func_decl_cursor)) {
                return "{}";
            }

            CXType const func_type =
                deduce_func_decl_type_at_cursor(func_decl_cursor);
            if (func_type.kind == CXType_Invalid) {
                return "{}";
            }

            std::string result;
            result += stringize_type(func_type);
            result += "'canonical':{" +
                      stringize_type(clang_getCanonicalType(func_type)) + "},";
            return "{" + result + "}";
        });
}

const char* libclang_vim::deduce_type_at(const location_tuple& location_info) {
    return at_specific_location(
        location_info, [](CXCursor const& cursor) -> std::string {
            CXCursor valid_cursor = cursor;
            if (is_invalid_type_cursor(valid_cursor)) {
                clang_visitChildren(cursor, valid_type_cursor_getter,
                                    &valid_cursor);
            }
            if (is_invalid_type_cursor(valid_cursor)) {
                return "{}";
            }

            CXCursorKind const kind = clang_getCursorKind(valid_cursor);
            CXType const result_type =
                kind == CXCursor_VarDecl
                    ? deduce_type_at_cursor(valid_cursor)
                    : is_function_decl_kind(kind)
                          ? deduce_func_decl_type_at_cursor(valid_cursor)
                          : clang_getCursorType(valid_cursor);
            if (result_type.kind == CXType_Invalid) {
                return "{}";
            }

            std::string result;
            result += stringize_type(result_type);
            result += "'canonical':{" +
                      stringize_type(clang_getCanonicalType(result_type)) +
                      "},";
            return "{" + result + "}";
        });
}

const char* libclang_vim::get_compile_commands(const std::string& file) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "{'commands':'";

    args_type args = parse_compilation_database(file);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i)
            ss << " ";
        ss << args[i];
    }

    // Write the footer.
    ss << "'}";
    vimson = ss.str();
    return vimson.c_str();
}

const char*
libclang_vim::get_current_function_at(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "{'name':'";

    // Write the actual name.
    cxindex_ptr index = clang_createIndex(/*excludeDeclarationsFromPCH=*/1,
                                          /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    unsigned options = CXTranslationUnit_Incomplete;
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), options));
    if (!translation_unit)
        return "{}";

    CXFile file = clang_getFile(translation_unit, file_name.c_str());
    unsigned line = location_info.line;
    unsigned column = location_info.col;

    CXCursor cursor;
    CXCursorKind kind;
    while (true) {
        CXSourceLocation location =
            clang_getLocation(translation_unit, file, line, column);
        cursor = clang_getCursor(translation_unit, location);
        kind = clang_getCursorKind(cursor);
        if (!clang_isInvalid(
                clang_getCursorKind(clang_getCursorSemanticParent(cursor))) ||
            column <= 1)
            break;

        // This happens with e.g. CXCursor_TypeRef, work it around by going
        // back till we get a sane parent, if we can.
        --column;
    }

    while (true) {
        if (is_function_decl_kind(kind) || kind == CXCursor_TranslationUnit)
            break;
        cursor = clang_getCursorSemanticParent(cursor);
        kind = clang_getCursorKind(cursor);
    }

    if (kind != CXCursor_TranslationUnit) {
        std::stack<std::string> stack;
        while (true) {
            cxstring_ptr aString = clang_getCursorSpelling(cursor);
            if (!strlen(clang_getCString(aString)))
                stack.push("(anonymous namespace)");
            else
                stack.push(clang_getCString(aString));

            cursor = clang_getCursorSemanticParent(cursor);
            if (clang_getCursorKind(cursor) == CXCursor_TranslationUnit)
                break;
        }
        bool first = true;
        while (!stack.empty()) {
            if (first)
                first = false;
            else
                ss << "::";
            ss << stack.top();
            stack.pop();
        }
    }

    // Write the footer.
    ss << "'}";
    vimson = ss.str();
    return vimson.c_str();
}

const char* libclang_vim::get_comment_at(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "{'brief':'";

    // Write the actual comment.
    cxindex_ptr index = clang_createIndex(/*excludeDeclarationsFromPCH=*/1,
                                          /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    unsigned options = CXTranslationUnit_Incomplete;
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), options));
    if (!translation_unit)
        return "{}";

    CXFile file = clang_getFile(translation_unit, file_name.c_str());
    int line = location_info.line;
    int column = location_info.col;
    CXSourceLocation source_location =
        clang_getLocation(translation_unit, file, line, column);
    CXCursor cursor = clang_getCursor(translation_unit, source_location);
    if (clang_Cursor_isNull(cursor) ||
        clang_isInvalid(clang_getCursorKind(cursor)))
        return "{}";

    CXCursor referenced_cursor = clang_getCursorReferenced(cursor);
    if (!clang_Cursor_isNull(referenced_cursor) &&
        !clang_isInvalid(clang_getCursorKind(referenced_cursor)))
        cursor = referenced_cursor;

    CXCursor canonical_cursor = clang_getCanonicalCursor(cursor);
    if (clang_Cursor_isNull(canonical_cursor) ||
        clang_isInvalid(clang_getCursorKind(canonical_cursor)))
        return "{}";

    cxstring_ptr brief = clang_Cursor_getBriefCommentText(canonical_cursor);
    if (clang_getCString(brief))
        ss << clang_getCString(brief);

    // Write the footer.
    ss << "'}";
    vimson = ss.str();
    return vimson.c_str();
}

const char*
libclang_vim::get_deduced_declaration_at(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "{";

    // Write the actual comment.
    cxindex_ptr index = clang_createIndex(/*excludeDeclarationsFromPCH=*/1,
                                          /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    CXTranslationUnit_Flags flags = CXTranslationUnit_Incomplete;
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), flags));
    if (!translation_unit)
        return "{}";

    CXFile file = clang_getFile(translation_unit, file_name.c_str());
    int line = location_info.line;
    int column = location_info.col;
    CXSourceLocation source_location =
        clang_getLocation(translation_unit, file, line, column);
    CXCursor cursor = clang_getCursor(translation_unit, source_location);
    if (clang_Cursor_isNull(cursor) ||
        clang_isInvalid(clang_getCursorKind(cursor)))
        return "{}";

    CXCursor referenced_cursor = clang_getCursorReferenced(cursor);
    if (clang_Cursor_isNull(referenced_cursor) &&
        clang_isInvalid(clang_getCursorKind(referenced_cursor)))
        return "{}";

    CXCursor canonical_cursor = clang_getCanonicalCursor(referenced_cursor);
    if (clang_Cursor_isNull(canonical_cursor) ||
        clang_isInvalid(clang_getCursorKind(canonical_cursor)))
        return "{}";

    CXSourceLocation declaration_location =
        clang_getCursorLocation(canonical_cursor);
    CXFile declaration_file;
    unsigned declaration_line;
    unsigned declaration_col;
    clang_getExpansionLocation(declaration_location, &declaration_file,
                               &declaration_line, &declaration_col, nullptr);
    cxstring_ptr declaration_file_name = clang_getFileName(declaration_file);
    ss << "'file':'" << clang_getCString(declaration_file_name) << "',";
    ss << "'line':'" << declaration_line << "',";
    ss << "'col':'" << declaration_col << "',";

    // Write the footer.
    ss << "}";
    vimson = ss.str();
    return vimson.c_str();
}

const char* libclang_vim::get_include_at(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "{'file':'";

    // Write the actual comment.
    cxindex_ptr index = clang_createIndex(/*excludeDeclarationsFromPCH=*/1,
                                          /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    unsigned options = CXTranslationUnit_Incomplete |
                       CXTranslationUnit_DetailedPreprocessingRecord;
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), options));
    if (!translation_unit)
        return "{}";

    CXFile file = clang_getFile(translation_unit, file_name.c_str());
    int line = location_info.line;
    int column = location_info.col;
    CXSourceLocation source_location =
        clang_getLocation(translation_unit, file, line, column);
    CXCursor cursor = clang_getCursor(translation_unit, source_location);
    if (clang_getCursorKind(cursor) != CXCursor_InclusionDirective)
        return "{}";

    CXFile included_file = clang_getIncludedFile(cursor);
    cxstring_ptr included_name = clang_getFileName(included_file);
    ss << clang_getCString(included_name);

    // Write the footer.
    ss << "'}";
    vimson = ss.str();
    return vimson.c_str();
}

const char*
libclang_vim::get_completion_at(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "['";

    // Write the completion list.
    libclang_vim::cxindex_ptr index = clang_createIndex(
        /*excludeDeclarationsFromPCH=*/1, /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    unsigned options = CXTranslationUnit_Incomplete;
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), options));
    if (!translation_unit)
        return "[]";

    unsigned line = location_info.line;
    unsigned column = location_info.col;
    CXCodeCompleteResults* results = clang_codeCompleteAt(
        translation_unit, file_name.c_str(), line, column, unsaved_files.data(),
        unsaved_files.size(), clang_defaultCodeCompleteOptions());
    std::set<std::string> matches;
    if (results) {
        for (unsigned i = 0; i < results->NumResults; ++i) {
            const CXCompletionString& completion_string =
                results->Results[i].CompletionString;
            std::stringstream match;
            for (unsigned j = 0;
                 j < clang_getNumCompletionChunks(completion_string); ++j) {
                if (clang_getCompletionChunkKind(completion_string, j) !=
                    CXCompletionChunk_TypedText)
                    continue;

                const CXString& chunk_text =
                    clang_getCompletionChunkText(completion_string, j);
                match << clang_getCString(chunk_text);
            }
            matches.insert(match.str());
        }
        clang_disposeCodeCompleteResults(results);
    }
    for (auto it = matches.begin(); it != matches.end(); ++it) {
        if (it != matches.begin())
            ss << "', '";
        ss << *it;
    }

    // Write the footer.
    ss << "']";
    vimson = ss.str();
    return vimson.c_str();
}

const char* libclang_vim::get_diagnostics(const location_tuple& location_info) {
    static std::string vimson;

    // Write the header.
    std::stringstream ss;
    ss << "[";

    // Write the diagnostic list.
    libclang_vim::cxindex_ptr index = clang_createIndex(
        /*excludeDeclarationsFromPCH=*/1, /*displayDiagnostics=*/0);

    std::string file_name = location_info.file;
    std::vector<const char*> args_ptrs = get_args_ptrs(location_info.args);
    std::vector<CXUnsavedFile> unsaved_files =
        create_unsaved_files(location_info);
    unsigned options = CXTranslationUnit_Incomplete;
    cxtranslation_unit_ptr translation_unit(clang_parseTranslationUnit(
        index, file_name.c_str(), args_ptrs.data(), args_ptrs.size(),
        unsaved_files.data(), unsaved_files.size(), options));
    if (!translation_unit)
        return "[]";

    unsigned num_diagnostics = clang_getNumDiagnostics(translation_unit);
    for (unsigned i = 0; i < num_diagnostics; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnostic(translation_unit, i);
        if (diagnostic) {
            std::string severity;
            switch (clang_getDiagnosticSeverity(diagnostic)) {
            case CXDiagnostic_Ignored:
                severity = "ignored";
                break;
            case CXDiagnostic_Note:
                severity = "note";
                break;
            case CXDiagnostic_Warning:
                severity = "warning";
                break;
            case CXDiagnostic_Error:
                severity = "error";
                break;
            case CXDiagnostic_Fatal:
                severity = "fatal";
                break;
            }
            ss << "{'severity': '" << severity << "', ";

            CXSourceLocation location = clang_getDiagnosticLocation(diagnostic);
            CXFile location_file;
            unsigned location_line;
            unsigned location_column;
            clang_getExpansionLocation(location, &location_file, &location_line,
                                       &location_column, nullptr);
            libclang_vim::cxstring_ptr location_file_name =
                clang_getFileName(location_file);
            ss << libclang_vim::stringize_location(location) << "}, ";
        }
        clang_disposeDiagnostic(diagnostic);
    }

    // Write the footer.
    ss << "]";
    vimson = ss.str();
    return vimson.c_str();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
