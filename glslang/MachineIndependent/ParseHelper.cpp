//
//Copyright (C) 2002-2005  3Dlabs Inc. Ltd.
//Copyright (C) 2012-2013 LunarG, Inc.
//
//All rights reserved.
//
//Redistribution and use in source and binary forms, with or without
//modification, are permitted provided that the following conditions
//are met:
//
//    Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//    Neither the name of 3Dlabs Inc. Ltd. nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
//THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
//FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
//COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
//BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
//ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//POSSIBILITY OF SUCH DAMAGE.
//

#include "ParseHelper.h"

#include "osinclude.h"
#include <stdarg.h>
#include <algorithm>

#include "preprocessor/PpContext.h"

extern int yyparse(void*);

namespace glslang {

TParseContext::TParseContext(TSymbolTable& symt, TIntermediate& interm, bool pb, int v, EProfile p, EShLanguage L, TInfoSink& is,
                             bool fc, EShMessages m) :
            intermediate(interm), symbolTable(symt), infoSink(is), language(L),
            version(v), profile(p), forwardCompatible(fc), messages(m),
            contextPragma(true, false), loopNestingLevel(0), structNestingLevel(0),
            tokensBeforeEOF(false),
            numErrors(0), parsingBuiltins(pb), afterEOF(false)
{
    currentLoc.line = 1;
    currentLoc.string = 0;

    // ensure we always have a linkage node, even if empty, to simplify tree topology algorithms
    linkage = new TIntermAggregate;

    // set all precision defaults to EpqNone, which is correct for all desktop types
    // and for ES types that don't have defaults (thus getting an error on use)
    for (int type = 0; type < EbtNumTypes; ++type)
        defaultPrecision[type] = EpqNone;

    for (int type = 0; type < maxSamplerIndex; ++type)
        defaultSamplerPrecision[type] = EpqNone;

    // replace with real defaults for those that have them
    if (profile == EEsProfile) {
        TSampler sampler;
        sampler.set(EbtFloat, Esd2D);
        defaultSamplerPrecision[computeSamplerTypeIndex(sampler)] = EpqLow;
        sampler.set(EbtFloat, EsdCube);
        defaultSamplerPrecision[computeSamplerTypeIndex(sampler)] = EpqLow;

        switch (language) {
        case EShLangVertex:
            defaultPrecision[EbtInt] = EpqHigh;
            defaultPrecision[EbtUint] = EpqHigh;
            defaultPrecision[EbtFloat] = EpqHigh;
            defaultPrecision[EbtSampler] = EpqLow;
            break;
        case EShLangFragment:
            defaultPrecision[EbtInt] = EpqMedium;
            defaultPrecision[EbtUint] = EpqMedium;
            defaultPrecision[EbtSampler] = EpqLow;
            break;
        default:
            infoSink.info.message(EPrefixError, "unexpected es-profile stage");
        }
    }

    globalUniformDefaults.clear();
    globalUniformDefaults.layoutMatrix = ElmColumnMajor;
    globalUniformDefaults.layoutPacking = ElpShared;

    globalBufferDefaults.clear();
    globalBufferDefaults.layoutMatrix = ElmColumnMajor;
    globalBufferDefaults.layoutPacking = ElpShared;

    globalInputDefaults.clear();

    globalOutputDefaults.clear();
}

//
// Parse an array of strings using yyparse, going through the
// preprocessor to tokenize the shader strings, then through
// the GLSL scanner.
//
// Returns true for successful acceptance of the shader, false if any errors.
//
bool TParseContext::parseShaderStrings(TPpContext& ppContext, char* strings[], size_t lengths[], int numStrings)
{
    // empty shaders are okay
    if (! strings || numStrings == 0 || lengths[0] == 0)
        return true;

    for (int i = 0; i < numStrings; ++i) {
        if (! strings[i]) {
            TSourceLoc loc;
            loc.string = i;
            loc.line = 1;
            error(loc, "Null shader source string", "", "");

            return false;
        }
    }

    if (getPreamble())
        ppContext.setPreamble(getPreamble(), strlen(getPreamble()));
    ppContext.setShaderStrings(strings, lengths, numStrings);

    // TODO: desktop PP: a shader containing nothing but white space and comments is valid, even though it has no parse tokens
    size_t len = 0;
    while (strings[0][len] == ' '  ||
           strings[0][len] == '\t' ||
           strings[0][len] == '\n' ||
           strings[0][len] == '\r') {
        if (++len >= lengths[0])
            return true;
    }

    anyIndexLimits = ! limits.generalAttributeMatrixVectorIndexing ||
                     ! limits.generalConstantMatrixVectorIndexing ||
                     ! limits.generalSamplerIndexing ||
                     ! limits.generalUniformIndexing ||
                     ! limits.generalVariableIndexing ||
                     ! limits.generalVaryingIndexing;

    yyparse((void*)this);

    finalize();

    return numErrors == 0;
}

// This is called from bison when it has a parse (syntax) error
void TParseContext::parserError(const char *s)
{
    if (afterEOF) {
        if (tokensBeforeEOF == 1)
            error(currentLoc, "", "pre-mature EOF", s, "");
    } else
        error(currentLoc, "", "", s, "");
}

void TParseContext::handlePragma(const char **tokens, int numTokens)
{
    if (!strcmp(tokens[0], "optimize")) {
        if (numTokens != 4) {
            error(currentLoc, "optimize pragma syntax is incorrect", "#pragma", "");
            return;
        }

        if (strcmp(tokens[1], "(")) {
            error(currentLoc, "\"(\" expected after 'optimize' keyword", "#pragma", "");
            return;
        }

        if (!strcmp(tokens[2], "on"))
            contextPragma.optimize = true;
        else if (!strcmp(tokens[2], "off"))
            contextPragma.optimize = false;
        else {
            error(currentLoc, "\"on\" or \"off\" expected after '(' for 'optimize' pragma", "#pragma", "");
            return;
        }

        if (strcmp(tokens[3], ")")) {
            error(currentLoc, "\")\" expected to end 'optimize' pragma", "#pragma", "");
            return;
        }
    } else if (!strcmp(tokens[0], "debug")) {
        if (numTokens != 4) {
            error(currentLoc, "debug pragma syntax is incorrect", "#pragma", "");
            return;
        }

        if (strcmp(tokens[1], "(")) {
            error(currentLoc, "\"(\" expected after 'debug' keyword", "#pragma", "");
            return;
        }

        if (!strcmp(tokens[2], "on"))
            contextPragma.debug = true;
        else if (!strcmp(tokens[2], "off"))
            contextPragma.debug = false;
        else {
            error(currentLoc, "\"on\" or \"off\" expected after '(' for 'debug' pragma", "#pragma", "");
            return;
        }

        if (strcmp(tokens[3], ")")) {
            error(currentLoc, "\")\" expected to end 'debug' pragma", "#pragma", "");
            return;
        }
    } else {

#ifdef PRAGMA_TABLE
        //
        // implementation specific pragma
        // use parseContext.contextPragma.pragmaTable to store the information about pragma
        // For now, just ignore the pragma that the implementation cannot recognize
        // An Example of one such implementation for a pragma that has a syntax like
        // #pragma pragmaname(pragmavalue)
        // This implementation stores the current pragmavalue against the pragma name in pragmaTable.
        //
        if (numTokens == 4 && !strcmp(tokens[1], "(") && !strcmp(tokens[3], ")")) {
            TPragmaTable& pragmaTable = parseContext.contextPragma.pragmaTable;
            TPragmaTable::iterator iter;
            iter = pragmaTable.find(TString(tokens[0]));
            if (iter != pragmaTable.end()) {
                iter->second = tokens[2];
            } else {
                pragmaTable[tokens[0]] = tokens[2];
            }
        } else if (numTokens >= 2) {
            TPragmaTable& pragmaTable = parseContext.contextPragma.pragmaTable;
            TPragmaTable::iterator iter;
            iter = pragmaTable.find(TString(tokens[0]));
            if (iter != pragmaTable.end()) {
                iter->second = tokens[1];
            } else {
                pragmaTable[tokens[0]] = tokens[1];
            }
        }
#endif // PRAGMA_TABLE
    }
}

///////////////////////////////////////////////////////////////////////
//
// Sub- vector and matrix fields
//
////////////////////////////////////////////////////////////////////////

//
// Look at a '.' field selector string and change it into offsets
// for a vector or scalar
//
// Returns true if there is no error.
//
bool TParseContext::parseVectorFields(TSourceLoc loc, const TString& compString, int vecSize, TVectorFields& fields)
{
    fields.num = (int) compString.size();
    if (fields.num > 4) {
        error(loc, "illegal vector field selection", compString.c_str(), "");
        return false;
    }

    enum {
        exyzw,
        ergba,
        estpq,
    } fieldSet[4];

    for (int i = 0; i < fields.num; ++i) {
        switch (compString[i])  {
        case 'x':
            fields.offsets[i] = 0;
            fieldSet[i] = exyzw;
            break;
        case 'r':
            fields.offsets[i] = 0;
            fieldSet[i] = ergba;
            break;
        case 's':
            fields.offsets[i] = 0;
            fieldSet[i] = estpq;
            break;
        case 'y':
            fields.offsets[i] = 1;
            fieldSet[i] = exyzw;
            break;
        case 'g':
            fields.offsets[i] = 1;
            fieldSet[i] = ergba;
            break;
        case 't':
            fields.offsets[i] = 1;
            fieldSet[i] = estpq;
            break;
        case 'z':
            fields.offsets[i] = 2;
            fieldSet[i] = exyzw;
            break;
        case 'b':
            fields.offsets[i] = 2;
            fieldSet[i] = ergba;
            break;
        case 'p':
            fields.offsets[i] = 2;
            fieldSet[i] = estpq;
            break;

        case 'w':
            fields.offsets[i] = 3;
            fieldSet[i] = exyzw;
            break;
        case 'a':
            fields.offsets[i] = 3;
            fieldSet[i] = ergba;
            break;
        case 'q':
            fields.offsets[i] = 3;
            fieldSet[i] = estpq;
            break;
        default:
            error(loc, "illegal vector field selection", compString.c_str(), "");
            return false;
        }
    }

    for (int i = 0; i < fields.num; ++i) {
        if (fields.offsets[i] >= vecSize) {
            error(loc, "vector field selection out of range",  compString.c_str(), "");
            return false;
        }

        if (i > 0) {
            if (fieldSet[i] != fieldSet[i-1]) {
                error(loc, "illegal - vector component fields not from the same set", compString.c_str(), "");
                return false;
            }
        }
    }

    return true;
}

///////////////////////////////////////////////////////////////////////
//
// Errors
//
////////////////////////////////////////////////////////////////////////

//
// Used to output syntax, parsing, and semantic errors.
//
void C_DECL TParseContext::error(TSourceLoc loc, const char *szReason, const char *szToken,
                                 const char *szExtraInfoFormat, ...)
{
    const int maxSize = GlslangMaxTokenLength + 200;
    char szExtraInfo[maxSize];
    va_list marker;

    va_start(marker, szExtraInfoFormat);

    safe_vsprintf(szExtraInfo, maxSize, szExtraInfoFormat, marker);

    infoSink.info.prefix(EPrefixError);
    infoSink.info.location(loc);
    infoSink.info << "'" << szToken <<  "' : " << szReason << " " << szExtraInfo << "\n";

    va_end(marker);

    ++numErrors;
}

void C_DECL TParseContext::warn(TSourceLoc loc, const char *szReason, const char *szToken,
                                 const char *szExtraInfoFormat, ...)
{
    if (messages & EShMsgSuppressWarnings)
        return;

    const int maxSize = GlslangMaxTokenLength + 200;
    char szExtraInfo[maxSize];
    va_list marker;

    va_start(marker, szExtraInfoFormat);

    safe_vsprintf(szExtraInfo, maxSize, szExtraInfoFormat, marker);

    infoSink.info.prefix(EPrefixWarning);
    infoSink.info.location(loc);
    infoSink.info << "'" << szToken <<  "' : " << szReason << " " << szExtraInfo << "\n";

    va_end(marker);
}

//
// Handle seeing a variable identifier in the grammar.
//
TIntermTyped* TParseContext::handleVariable(TSourceLoc loc, TSymbol* symbol, TString* string)
{
    TIntermTyped* node = 0;

    const TAnonMember* anon = symbol ? symbol->getAsAnonMember() : 0;
    if (anon) {
        // it was a member of an anonymous container, have to insert its dereference
        const TVariable* variable = anon->getAnonContainer().getAsVariable();
        TIntermTyped* container = intermediate.addSymbol(variable->getUniqueId(), variable->getName(), variable->getType(), loc);
        TConstUnionArray unionArray(1);
        unionArray[0].setUConst(anon->getMemberNumber());
        TIntermTyped* constNode = intermediate.addConstantUnion(unionArray, TType(EbtUint, EvqConst), loc);

        node = intermediate.addIndex(EOpIndexDirectStruct, container, constNode, loc);
        node->setType(*(*variable->getType().getStruct())[anon->getMemberNumber()].type);
    } else {
        // The symbol table search was done in the lexical phase, but
        // if this is a new symbol, it wouldn't have found it.
        const TVariable* variable = symbol ? symbol->getAsVariable() : 0;
        if (symbol && ! variable)
            error(loc, "variable name expected", string->c_str(), "");

        if (! variable)
            variable = new TVariable(string, TType(EbtVoid));

        // don't delete $1.string, it's used by error recovery, and the pool
        // pop will reclaim the memory

        if (variable->getType().getQualifier().storage == EvqConst)
            node = intermediate.addConstantUnion(variable->getConstArray(), variable->getType(), loc);
        else
            node = intermediate.addSymbol(variable->getUniqueId(), variable->getName(), variable->getType(), loc);
    }

    return node;
}

//
// Handle seeing a base[index] dereference in the grammar.
//
TIntermTyped* TParseContext::handleBracketDereference(TSourceLoc loc, TIntermTyped* base, TIntermTyped* index)
{
    TIntermTyped* result = 0;

    variableCheck(base);
    if (! base->isArray() && ! base->isMatrix() && ! base->isVector()) {
        if (base->getAsSymbolNode())
            error(loc, " left of '[' is not of type array, matrix, or vector ", base->getAsSymbolNode()->getName().c_str(), "");
        else
            error(loc, " left of '[' is not of type array, matrix, or vector ", "expression", "");
    } else if (base->getType().getQualifier().storage == EvqConst && index->getQualifier().storage == EvqConst) {
        if (base->isArray()) {
            // constant folding for arrays
            result = addConstArrayNode(index->getAsConstantUnion()->getConstArray()[0].getIConst(), base, loc);
        } else if (base->isVector()) {
            // constant folding for vectors
            TVectorFields fields;
            fields.num = 1;
            fields.offsets[0] = index->getAsConstantUnion()->getConstArray()[0].getIConst(); // need to do it this way because v.xy sends fields integer array
            result = addConstVectorNode(fields, base, loc);
        } else if (base->isMatrix()) {
            // constant folding for matrices
            result = addConstMatrixNode(index->getAsConstantUnion()->getConstArray()[0].getIConst(), base, loc);
        }
    } else {
        if (index->getQualifier().storage == EvqConst) {
            int indexValue = index->getAsConstantUnion()->getConstArray()[0].getIConst();
            if (! base->isArray() && ((base->isVector() && base->getType().getVectorSize() <= indexValue) ||
                                      (base->isMatrix() && base->getType().getMatrixCols() <= indexValue)))
                error(loc, "", "[", "index out of range '%d'", index->getAsConstantUnion()->getConstArray()[0].getIConst());
            if (base->isArray()) {
                if (base->getType().getArraySize() == 0)
                    updateMaxArraySize(loc, base, index->getAsConstantUnion()->getConstArray()[0].getIConst());
                else if (index->getAsConstantUnion()->getConstArray()[0].getIConst() >= base->getType().getArraySize() ||
                           index->getAsConstantUnion()->getConstArray()[0].getIConst() < 0)
                    error(loc, "", "[", "array index out of range '%d'", index->getAsConstantUnion()->getConstArray()[0].getIConst());
            }
            result = intermediate.addIndex(EOpIndexDirect, base, index, loc);
        } else {
            if (base->isArray() && base->getType().getArraySize() == 0)
                error(loc, "", "[", "array must be redeclared with a size before being indexed with a variable");
            if (base->getBasicType() == EbtBlock)
                requireProfile(base->getLoc(), ~EEsProfile, "variable indexing block array");
            if (base->getBasicType() == EbtSampler && version >= 130) {
                const char* explanation = "variable indexing sampler array";
                requireProfile(base->getLoc(), ECoreProfile | ECompatibilityProfile, explanation);
                profileRequires(base->getLoc(), ECoreProfile | ECompatibilityProfile, 400, 0, explanation);
            }

            result = intermediate.addIndex(EOpIndexIndirect, base, index, loc);
        }
    }

    if (result == 0) {
        TConstUnionArray unionArray(1);
        unionArray[0].setDConst(0.0);
        result = intermediate.addConstantUnion(unionArray, TType(EbtFloat, EvqConst), loc);
    } else {
        TType newType;
        newType.shallowCopy(base->getType());
        if (base->getType().getQualifier().storage == EvqConst && index->getQualifier().storage == EvqConst)
            newType.getQualifier().storage = EvqConst;
        newType.dereference();
        result->setType(newType);

        if (anyIndexLimits) {
            // for ES 2.0 (version 100) limitations for almost all index operations except vertex-shader uniforms
            if ((! limits.generalSamplerIndexing && base->getBasicType() == EbtSampler) ||
                (! limits.generalUniformIndexing && base->getQualifier().isUniform() && language != EShLangVertex) ||
                (! limits.generalAttributeMatrixVectorIndexing && base->getQualifier().isPipeInput() && language == EShLangVertex && (base->getType().isMatrix() || base->getType().isVector())) ||
                (! limits.generalConstantMatrixVectorIndexing && base->getAsConstantUnion()) ||
                (! limits.generalVariableIndexing && ! base->getType().getQualifier().isUniform() && 
                                                     ! base->getType().getQualifier().isPipeInput() && 
                                                     ! base->getType().getQualifier().isPipeOutput() &&
                                                       base->getType().getQualifier().storage != EvqConst) ||
                (! limits.generalVaryingIndexing && (base->getType().getQualifier().isPipeInput() || 
                                                     base->getType().getQualifier().isPipeOutput()))) {            
                // it's too early to know what the inductive variables are, save it for post processing
                needsIndexLimitationChecking.push_back(index);
            }
        }
    }

    return result;
}

//
// Handle seeing a base.field dereference in the grammar.
//
TIntermTyped* TParseContext::handleDotDereference(TSourceLoc loc, TIntermTyped* base, TString& field)
{
    TIntermTyped* result = base;

    variableCheck(base);
    if (base->isArray()) {
        //
        // It can only be a method (e.g., length), which can't be resolved until
        // we later see the function calling syntax.  Save away the name for now.
        //

        if (field == "length") {
            profileRequires(loc, ENoProfile, 120, GL_3DL_array_objects, ".length");
            profileRequires(loc, EEsProfile, 300, 0, ".length");
            result = intermediate.addMethod(base, TType(EbtInt), &field, loc);
        } else
            error(loc, "only the length method is supported for array", field.c_str(), "");
    } else if (base->isVector() || base->isScalar()) {
        if (base->isScalar()) {
            const char* dotFeature = "scalar swizzle";
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, dotFeature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 420, GL_ARB_shading_language_420pack, dotFeature);
        }

        TVectorFields fields;
        if (! parseVectorFields(loc, field, base->getVectorSize(), fields)) {
            fields.num = 1;
            fields.offsets[0] = 0;
        }

        if (base->isScalar()) {
            if (fields.num == 1)
                return result;
            else {
                TType type(base->getBasicType(), EvqTemporary, fields.num);
                return addConstructor(loc, base, type, mapTypeToConstructorOp(type));
            }
        }

        if (base->getType().getQualifier().storage == EvqConst) { // constant folding for vector fields
            result = addConstVectorNode(fields, base, loc);
            if (result == 0)
                result = base;
            else
                result->setType(TType(base->getBasicType(), EvqConst, (int) (field).size()));
        } else {
            if (fields.num == 1) {
                TConstUnionArray unionArray(1);
                unionArray[0].setIConst(fields.offsets[0]);
                TIntermTyped* index = intermediate.addConstantUnion(unionArray, TType(EbtInt, EvqConst), loc);
                result = intermediate.addIndex(EOpIndexDirect, base, index, loc);
                result->setType(TType(base->getBasicType(), EvqTemporary, base->getType().getQualifier().precision));
            } else {
                TString vectorString = field;
                TIntermTyped* index = intermediate.addSwizzle(fields, loc);
                result = intermediate.addIndex(EOpVectorSwizzle, base, index, loc);
                result->setType(TType(base->getBasicType(), EvqTemporary, base->getType().getQualifier().precision, (int) vectorString.size()));
            }
        }
    } else if (base->isMatrix())
        error(loc, "field selection not allowed on matrix", ".", "");
    else if (base->getBasicType() == EbtStruct || base->getBasicType() == EbtBlock) {
        bool fieldFound = false;
        TTypeList* fields = base->getType().getStruct();
        if (fields == 0)
            error(loc, "structure has no fields", "Internal Error", "");
        else {
            unsigned int i;
            for (i = 0; i < fields->size(); ++i) {
                if ((*fields)[i].type->getFieldName() == field) {
                    fieldFound = true;
                    break;
                }
            }
            if (fieldFound) {
                if (base->getType().getQualifier().storage == EvqConst) {
                    result = addConstStruct(field, base, loc);
                    if (result == 0)
                        result = base;
                    else {
                        result->setType(*(*fields)[i].type);
                        // change the qualifier of the return type, not of the structure field
                        // as the structure definition is shared between various structures.
                        result->getWritableType().getQualifier().storage = EvqConst;
                    }
                } else {
                    TConstUnionArray unionArray(1);
                    unionArray[0].setIConst(i);
                    TIntermTyped* index = intermediate.addConstantUnion(unionArray, TType(EbtInt, EvqConst), loc);
                    result = intermediate.addIndex(EOpIndexDirectStruct, base, index, loc);
                    result->setType(*(*fields)[i].type);
                }
            } else
                error(loc, " no such field in structure", field.c_str(), "");
        }
    } else
        error(loc, " dot operator does not operater on this type:", field.c_str(), base->getType().getCompleteString().c_str());

    return result;
}

//
// Handle seeing a function declarator in the grammar.  This is the precursor
// to recognizing a function prototype or function definition.
//
TFunction* TParseContext::handleFunctionDeclarator(TSourceLoc loc, TFunction& function)
{
    // ES can't declare prototypes inside functions
    if (! symbolTable.atGlobalLevel())
        requireProfile(loc, ~EEsProfile, "local function declaration");

    //
    // Multiple declarations of the same function are allowed.
    //
    // If this is a definition, the definition production code will check for redefinitions
    // (we don't know at this point if it's a definition or not).
    //
    // Redeclarations (full prototype match) are allowed.  But, return types and parameter qualifiers must match.
    //
    // ES does not allow redeclaring or hiding of built-in functions.
    //
    bool builtIn;
    TSymbol* symbol = symbolTable.find(function.getMangledName(), &builtIn);
    if (symbol && symbol->getAsFunction() && builtIn)
        requireNotRemoved(loc, EEsProfile, 300, "redeclaration of built-in function");
    const TFunction* prevDec = symbol ? symbol->getAsFunction() : 0;
    if (prevDec) {
        if (prevDec->getType() != function.getType()) {
            error(loc, "overloaded functions must have the same return type", function.getType().getCompleteTypeString().c_str(), "");
        }
        for (int i = 0; i < prevDec->getParamCount(); ++i) {
            if ((*prevDec)[i].type->getQualifier().storage != function[i].type->getQualifier().storage)
                error(loc, "overloaded functions must have the same parameter qualifiers", function[i].type->getStorageQualifierString(), "");
        }
    }


    if (! symbolTable.insert(function))
        error(loc, "illegal redeclaration", function.getName().c_str(), "");

    //
    // If this is a redeclaration, it could also be a definition,
    // in which case, we want to use the variable names from this one, and not the one that's
    // being redeclared.  So, pass back this declaration, not the one in the symbol table.
    //
    return &function;
}

//
// Handle seeing a function prototype in the grammar.  This includes what may
// become a full definition, as a full definition looks like a prototype
// followed by a body.  The body is handled after this function
// returns, when present.
//
TIntermAggregate* TParseContext::handleFunctionPrototype(TSourceLoc loc, TFunction& function)
{
    currentCaller = function.getMangledName();
    TSymbol* symbol = symbolTable.find(function.getMangledName());
    TFunction* prevDec = symbol ? symbol->getAsFunction() : 0;

    if (! prevDec)
        error(loc, "can't find function name", function.getName().c_str(), "");

    //
    // Note:  'prevDec' could be 'function' if this is the first time we've seen function
    // as it would have just been put in the symbol table.  Otherwise, we're looking up
    // an earlier occurance.
    //
    if (prevDec && prevDec->isDefined()) {
        //
        // Then this function already has a body.
        //
        error(loc, "function already has a body", function.getName().c_str(), "");
    }
    if (prevDec) {
        prevDec->setDefined();
        //
        // Remember the return type for later checking for RETURN statements.
        //
        currentFunctionType = &(prevDec->getType());
    } else
        currentFunctionType = new TType(EbtVoid);
    functionReturnsValue = false;

    //
    // Raise error message if main function takes any parameters or returns anything other than void
    //
    if (function.getName() == "main") {
        if (function.getParamCount() > 0)
            error(loc, "function cannot take any parameter(s)", function.getName().c_str(), "");
        if (function.getType().getBasicType() != EbtVoid)
            error(loc, "", function.getType().getCompleteTypeString().c_str(), "main function cannot return a value");
        intermediate.addMainCount();
    }

    //
    // New symbol table scope for body of function plus its arguments
    //
    symbolTable.push();

    //
    // Insert parameters into the symbol table.
    // If the parameter has no name, it's not an error, just don't insert it
    // (could be used for unused args).
    //
    // Also, accumulate the list of parameters into the HIL, so lower level code
    // knows where to find parameters.
    //
    TIntermAggregate* paramNodes = new TIntermAggregate;
    for (int i = 0; i < function.getParamCount(); i++) {
        TParameter& param = function[i];
        if (param.name != 0) {
            TVariable *variable = new TVariable(param.name, *param.type);
            //
            // Insert the parameters with name in the symbol table.
            //
            if (! symbolTable.insert(*variable)) {
                error(loc, "redefinition", variable->getName().c_str(), "");
                delete variable;
            }
            //
            // Transfer ownership of name pointer to symbol table.
            //
            param.name = 0;

            //
            // Add the parameter to the HIL
            //
            paramNodes = intermediate.growAggregate(paramNodes,
                                                    intermediate.addSymbol(variable->getUniqueId(),
                                                                            variable->getName(),
                                                                            variable->getType(), loc),
                                                    loc);
        } else
            paramNodes = intermediate.growAggregate(paramNodes, intermediate.addSymbol(0, "", *param.type, loc), loc);
    }
    intermediate.setAggregateOperator(paramNodes, EOpParameters, TType(EbtVoid), loc);
    loopNestingLevel = 0;

    return paramNodes;
}

//
// Handle seeing a function call in the grammar.
//
TIntermTyped* TParseContext::handleFunctionCall(TSourceLoc loc, TFunction* fnCall, TIntermNode* intermNode, TIntermAggregate* intermAggregate)
{
    TIntermTyped* result = 0;

    TOperator op = fnCall->getBuiltInOp();
    if (op == EOpArrayLength) {
        if (fnCall->getParamCount() > 0)
            error(loc, "method does not accept any arguments", fnCall->getName().c_str(), "");
        int length;
        if (intermNode->getAsTyped() == 0 || ! intermNode->getAsTyped()->getType().isArray() || intermNode->getAsTyped()->getType().getArraySize() == 0) {
            error(loc, "", fnCall->getName().c_str(), "array must be declared with a size before using this method");
            length = 1;
        } else
            length = intermNode->getAsTyped()->getType().getArraySize();

        TConstUnionArray unionArray(1);
        unionArray[0].setIConst(length);
        result = intermediate.addConstantUnion(unionArray, TType(EbtInt, EvqConst), loc);
    } else if (op != EOpNull) {
        //
        // Then this should be a constructor.
        // Don't go through the symbol table for constructors.
        // Their parameters will be verified algorithmically.
        //
        TType type(EbtVoid);  // use this to get the type back
        if (! constructorError(loc, intermNode, *fnCall, op, type)) {
            //
            // It's a constructor, of type 'type'.
            //
            result = addConstructor(loc, intermNode, type, op);
            if (result == 0)
                error(loc, "cannot construct with these arguments", type.getCompleteString().c_str(), "");
        }
    } else {
        //
        // Not a constructor.  Find it in the symbol table.
        //
        const TFunction* fnCandidate;
        bool builtIn;
        fnCandidate = findFunction(loc, fnCall, &builtIn);
        if (fnCandidate) {
            //
            // A declared function.  But, it might still map to a built-in
            // operation.
            //
            op = fnCandidate->getBuiltInOp();
            if (builtIn && op != EOpNull) {
                // A function call mapped to a built-in operation.
                result = intermediate.addBuiltInFunctionCall(loc, op, fnCandidate->getParamCount() == 1, intermNode, fnCandidate->getType());
                if (result == 0)  {
                    error(intermNode->getLoc(), " wrong operand type", "Internal Error",
                                        "built in unary operator function.  Type: %s",
                                        static_cast<TIntermTyped*>(intermNode)->getCompleteString().c_str());
                }
            } else {
                // This is a function call not mapped to built-in operation
                result = intermediate.setAggregateOperator(intermAggregate, EOpFunctionCall, fnCandidate->getType(), loc);
                result->getAsAggregate()->setName(fnCandidate->getMangledName());

                // this is how we know whether the given function is a built-in function or a user-defined function
                // if builtIn == false, it's a userDefined -> could be an overloaded built-in function also
                // if builtIn == true, it's definitely a built-in function with EOpNull
                if (! builtIn) {
                    result->getAsAggregate()->setUserDefined();
                    intermediate.addToCallGraph(infoSink, currentCaller, fnCandidate->getMangledName());
                }

                TStorageQualifier qual;
                TQualifierList& qualifierList = result->getAsAggregate()->getQualifierList();
                for (int i = 0; i < fnCandidate->getParamCount(); ++i) {
                    qual = (*fnCandidate)[i].type->getQualifier().storage;
                    if (qual == EvqOut || qual == EvqInOut) {
                        if (lValueErrorCheck(result->getLoc(), "assign", result->getAsAggregate()->getSequence()[i]->getAsTyped()))
                            error(intermNode->getLoc(), "Constant value cannot be passed for 'out' or 'inout' parameters.", "Error", "");
                    }
                    qualifierList.push_back(qual);
                }

                if (builtIn)
                    nonOpBuiltInCheck(loc, *fnCandidate, result->getAsAggregate());
            }
        } else {
            // error message was put out by PaFindFunction()
            // Put on a dummy node for error recovery
            TConstUnionArray unionArray(1);
            unionArray[0].setDConst(0.0);
            result = intermediate.addConstantUnion(unionArray, TType(EbtFloat, EvqConst), loc);
        }
    }

    // generic error recovery
    // TODO: coding: localize all the error recoveries that look like this
    if (result == 0) {
        TConstUnionArray unionArray(1);
        unionArray[0].setDConst(0.0);
        result = intermediate.addConstantUnion(unionArray, TType(EbtFloat, EvqConst), loc);
    }

    return result;
}

//
// Do additional checking of built-in function calls that were not mapped
// to built-in operations (e.g., texturing functions).
//
// Assumes there has been a semantically correct match to a built-in function.
//
void TParseContext::nonOpBuiltInCheck(TSourceLoc loc, const TFunction& fnCandidate, TIntermAggregate* callNode)
{
    // built-in texturing functions get their return value precision from the precision of the sampler
    if (fnCandidate.getType().getQualifier().precision == EpqNone &&
        fnCandidate.getParamCount() > 0 && fnCandidate[0].type->getBasicType() == EbtSampler)
        callNode->getQualifier().precision = callNode->getAsAggregate()->getSequence()[0]->getAsTyped()->getQualifier().precision;

    if (fnCandidate.getName().compare(0, 13, "textureGather") == 0) {
        const char* feature = "texture gather function";
        requireProfile(loc, ~EEsProfile, feature);
        profileRequires(loc, ~EEsProfile, 400, GL_ARB_texture_gather, feature);
        int lastArgIndex = fnCandidate.getParamCount() - 1;
        if (fnCandidate[lastArgIndex].type->getBasicType() == EbtInt && fnCandidate[lastArgIndex].type->isScalar()) {
            // the last integral argument to a texture gather must be a constant int between 0 and 3
            if (callNode->getSequence()[lastArgIndex]->getAsConstantUnion()) {
                int value = callNode->getSequence()[lastArgIndex]->getAsConstantUnion()->getConstArray()[0].getIConst();
                if (value < 0 || value > 3)
                    error(loc, "must be 0, 1, 2, or 3", "texture gather component", "");
            } else
                error(loc, "must be a constant", "texture gather component", "");
        }
    }
}

//
// Handle seeing a built-in-type constructor call in the grammar.
//
TFunction* TParseContext::handleConstructorCall(TSourceLoc loc, TPublicType& publicType)
{
    publicType.qualifier.precision = EpqNone;
    TType type(publicType);

    if (type.isArray()) {
        profileRequires(loc, ENoProfile, 120, GL_3DL_array_objects, "arrayed constructor");
        profileRequires(loc, EEsProfile, 300, 0, "arrayed constructor");
    }

    TOperator op = mapTypeToConstructorOp(type);

    if (op == EOpNull) {
        error(loc, "cannot construct this type", TType::getBasicString(publicType.basicType), "");
        op = EOpConstructFloat;
        publicType.basicType = EbtFloat;
        TType errorType(publicType);
        type.shallowCopy(errorType);
    }

    TString empty("");

    return new TFunction(&empty, type, op);
}

//
// Given a type, find what operation would construct it.
//
TOperator TParseContext::mapTypeToConstructorOp(const TType& type)
{
    if (type.getStruct())
        return EOpConstructStruct;

    TOperator op;
    switch (type.getBasicType()) {
    case EbtFloat:
        if (type.isMatrix()) {
            switch (type.getMatrixCols()) {
            case 2:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructMat2x2; break;
                case 3: op = EOpConstructMat2x3; break;
                case 4: op = EOpConstructMat2x4; break;
                default: break; // some compilers want this
                }
                break;
            case 3:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructMat3x2; break;
                case 3: op = EOpConstructMat3x3; break;
                case 4: op = EOpConstructMat3x4; break;
                default: break; // some compilers want this
                }
                break;
            case 4:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructMat4x2; break;
                case 3: op = EOpConstructMat4x3; break;
                case 4: op = EOpConstructMat4x4; break;
                default: break; // some compilers want this
                }
                break;
            default: break; // some compilers want this
            }
        } else {
            switch(type.getVectorSize()) {
            case 1: op = EOpConstructFloat; break;
            case 2: op = EOpConstructVec2;  break;
            case 3: op = EOpConstructVec3;  break;
            case 4: op = EOpConstructVec4;  break;
            default: break; // some compilers want this
            }
        }
        break;
    case EbtDouble:
        if (type.getMatrixCols()) {
            switch (type.getMatrixCols()) {
            case 2:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructDMat2x2; break;
                case 3: op = EOpConstructDMat2x3; break;
                case 4: op = EOpConstructDMat2x4; break;
                default: break; // some compilers want this
                }
                break;
            case 3:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructDMat3x2; break;
                case 3: op = EOpConstructDMat3x3; break;
                case 4: op = EOpConstructDMat3x4; break;
                default: break; // some compilers want this
                }
                break;
            case 4:
                switch (type.getMatrixRows()) {
                case 2: op = EOpConstructDMat4x2; break;
                case 3: op = EOpConstructDMat4x3; break;
                case 4: op = EOpConstructDMat4x4; break;
                default: break; // some compilers want this
                }
                break;
            }
        } else {
            switch(type.getVectorSize()) {
            case 1: op = EOpConstructDouble; break;
            case 2: op = EOpConstructDVec2;  break;
            case 3: op = EOpConstructDVec3;  break;
            case 4: op = EOpConstructDVec4;  break;
            default: break; // some compilers want this
            }
        }
        break;
    case EbtInt:
        switch(type.getVectorSize()) {
        case 1: op = EOpConstructInt;   break;
        case 2: op = EOpConstructIVec2; break;
        case 3: op = EOpConstructIVec3; break;
        case 4: op = EOpConstructIVec4; break;
        default: break; // some compilers want this
        }
        break;
    case EbtUint:
        switch(type.getVectorSize()) {
        case 1: op = EOpConstructUint;  break;
        case 2: op = EOpConstructUVec2; break;
        case 3: op = EOpConstructUVec3; break;
        case 4: op = EOpConstructUVec4; break;
        default: break; // some compilers want this
        }
        break;
    case EbtBool:
        switch(type.getVectorSize()) {
        case 1:  op = EOpConstructBool;  break;
        case 2:  op = EOpConstructBVec2; break;
        case 3:  op = EOpConstructBVec3; break;
        case 4:  op = EOpConstructBVec4; break;
        default: break; // some compilers want this
        }
        break;
    default: 
        op = EOpNull;
        break;
    }

    return op;
}

//
// Same error message for all places assignments don't work.
//
void TParseContext::assignError(TSourceLoc loc, const char* op, TString left, TString right)
{
    error(loc, "", op, "cannot convert from '%s' to '%s'",
          right.c_str(), left.c_str());
}

//
// Same error message for all places unary operations don't work.
//
void TParseContext::unaryOpError(TSourceLoc loc, const char* op, TString operand)
{
   error(loc, " wrong operand type", op,
          "no operation '%s' exists that takes an operand of type %s (or there is no acceptable conversion)",
          op, operand.c_str());
}

//
// Same error message for all binary operations don't work.
//
void TParseContext::binaryOpError(TSourceLoc loc, const char* op, TString left, TString right)
{
    error(loc, " wrong operand types:", op,
            "no operation '%s' exists that takes a left-hand operand of type '%s' and "
            "a right operand of type '%s' (or there is no acceptable conversion)",
            op, left.c_str(), right.c_str());
}

//
// A basic type of EbtVoid is a key that the name string was seen in the source, but
// it was not found as a variable in the symbol table.  If so, give the error
// message and insert a dummy variable in the symbol table to prevent future errors.
//
void TParseContext::variableCheck(TIntermTyped*& nodePtr)
{
    TIntermSymbol* symbol = nodePtr->getAsSymbolNode();
    if (! symbol)
        return;

    if (symbol->getType().getBasicType() == EbtVoid) {
        error(symbol->getLoc(), "undeclared identifier", symbol->getName().c_str(), "");

        // Add to symbol table to prevent future error messages on the same name

        TVariable* fakeVariable = new TVariable(&symbol->getName(), TType(EbtFloat));
        symbolTable.insert(*fakeVariable);

        // substitute a symbol node for this new variable
        nodePtr = intermediate.addSymbol(fakeVariable->getUniqueId(),
                                         fakeVariable->getName(),
                                         fakeVariable->getType(), symbol->getLoc());
    } else {
        switch (symbol->getQualifier().storage) {
        case EvqPointCoord:
            profileRequires(symbol->getLoc(), ENoProfile, 120, 0, "gl_PointCoord");
            break;
        default: break; // some compilers want this
        }
    }
}

//
// Both test and if necessary, spit out an error, to see if the node is really
// an l-value that can be operated on this way.
//
// Returns true if the was an error.
//
bool TParseContext::lValueErrorCheck(TSourceLoc loc, const char* op, TIntermTyped* node)
{
    TIntermSymbol* symNode = node->getAsSymbolNode();
    TIntermBinary* binaryNode = node->getAsBinaryNode();

    if (binaryNode) {
        bool errorReturn;

        switch(binaryNode->getOp()) {
        case EOpIndexDirect:
        case EOpIndexIndirect:
        case EOpIndexDirectStruct:
            return lValueErrorCheck(loc, op, binaryNode->getLeft());
        case EOpVectorSwizzle:
            errorReturn = lValueErrorCheck(loc, op, binaryNode->getLeft());
            if (!errorReturn) {
                int offset[4] = {0,0,0,0};

                TIntermTyped* rightNode = binaryNode->getRight();
                TIntermAggregate *aggrNode = rightNode->getAsAggregate();

                for (TIntermSequence::iterator p = aggrNode->getSequence().begin();
                                               p != aggrNode->getSequence().end(); p++) {
                    int value = (*p)->getAsTyped()->getAsConstantUnion()->getConstArray()[0].getIConst();
                    offset[value]++;
                    if (offset[value] > 1) {
                        error(loc, " l-value of swizzle cannot have duplicate components", op, "", "");

                        return true;
                    }
                }
            }

            return errorReturn;
        default:
            break;
        }
        error(loc, " l-value required", op, "", "");

        return true;
    }


    const char* symbol = 0;
    if (symNode != 0)
        symbol = symNode->getName().c_str();

    const char* message = 0;
    switch (node->getQualifier().storage) {
    case EvqConst:          message = "can't modify a const";        break;
    case EvqConstReadOnly:  message = "can't modify a const";        break;
    case EvqVaryingIn:      message = "can't modify shader input";   break;
    case EvqUniform:        message = "can't modify a uniform";      break;
    case EvqInstanceId:     message = "can't modify gl_InstanceID";  break;
    case EvqVertexId:       message = "can't modify gl_VertexID";    break;
    case EvqFace:           message = "can't modify gl_FrontFace";   break;
    case EvqFragCoord:      message = "can't modify gl_FragCoord";   break;
    case EvqPointCoord:     message = "can't modify gl_PointCoord";  break;
    default:

        //
        // Type that can't be written to?
        //
        switch (node->getBasicType()) {
        case EbtSampler:
            message = "can't modify a sampler";
            break;
        case EbtVoid:
            message = "can't modify void";
            break;
        default:
            break;
        }
    }

    if (message == 0 && binaryNode == 0 && symNode == 0) {
        error(loc, " l-value required", op, "", "");

        return true;
    }


    //
    // Everything else is okay, no error.
    //
    if (message == 0)
        return false;

    //
    // If we get here, we have an error and a message.
    //
    if (symNode)
        error(loc, " l-value required", op, "\"%s\" (%s)", symbol, message);
    else
        error(loc, " l-value required", op, "(%s)", message);

    return true;
}

//
// Both test, and if necessary spit out an error, to see if the node is really
// a constant.
//
void TParseContext::constantValueCheck(TIntermTyped* node, const char* token)
{
    if (node->getQualifier().storage != EvqConst)
        error(node->getLoc(), "constant expression required", token, "");
}

//
// Both test, and if necessary spit out an error, to see if the node is really
// an integer.
//
void TParseContext::integerCheck(TIntermTyped* node, const char* token)
{
    if ((node->getBasicType() == EbtInt || node->getBasicType() == EbtUint) && node->isScalar())
        return;

    error(node->getLoc(), "scalar integer expression required", token, "");
}

//
// Both test, and if necessary spit out an error, to see if we are currently
// globally scoped.
//
void TParseContext::globalCheck(TSourceLoc loc, const char* token)
{
    if (! symbolTable.atGlobalLevel())
        error(loc, "not allowed in nested scope", token, "");
}

//
// If it starts "gl_" or has double underscore, it's a reserved name.
// Except, if the symbol table is at a built-in level,
// which is when we are parsing built-ins.
//
bool TParseContext::reservedErrorCheck(TSourceLoc loc, const TString& identifier)
{
    if (! symbolTable.atBuiltInLevel()) {
        if (identifier.compare(0, 3, "gl_") == 0) {
            error(loc, "reserved built-in name", "gl_", "");

            return true;
        }
        if (identifier.find("__") != TString::npos) {
            error(loc, "Two consecutive underscores are reserved for future use.", identifier.c_str(), "", "");

            return true;
        }
    }

    return false;
}

//
// Make sure there is enough data provided to the constructor to build
// something of the type of the constructor.  Also returns the type of
// the constructor.
//
// Returns true if there was an error in construction.
//
bool TParseContext::constructorError(TSourceLoc loc, TIntermNode* node, TFunction& function, TOperator op, TType& type)
{
    type.shallowCopy(function.getType());

    bool constructingMatrix = false;
    switch(op) {
    case EOpConstructMat2x2:
    case EOpConstructMat2x3:
    case EOpConstructMat2x4:
    case EOpConstructMat3x2:
    case EOpConstructMat3x3:
    case EOpConstructMat3x4:
    case EOpConstructMat4x2:
    case EOpConstructMat4x3:
    case EOpConstructMat4x4:
    case EOpConstructDMat2x2:
    case EOpConstructDMat2x3:
    case EOpConstructDMat2x4:
    case EOpConstructDMat3x2:
    case EOpConstructDMat3x3:
    case EOpConstructDMat3x4:
    case EOpConstructDMat4x2:
    case EOpConstructDMat4x3:
    case EOpConstructDMat4x4:
        constructingMatrix = true;
        break;
    default:
        break;
    }

    //
    // Note: It's okay to have too many components available, but not okay to have unused
    // arguments.  'full' will go to true when enough args have been seen.  If we loop
    // again, there is an extra argument, so 'overfull' will become true.
    //

    int size = 0;
    bool constType = true;
    bool full = false;
    bool overFull = false;
    bool matrixInMatrix = false;
    bool arrayArg = false;
    for (int i = 0; i < function.getParamCount(); ++i) {
        size += function[i].type->getObjectSize();

        if (constructingMatrix && function[i].type->isMatrix())
            matrixInMatrix = true;
        if (full)
            overFull = true;
        if (op != EOpConstructStruct && ! type.isArray() && size >= type.getObjectSize())
            full = true;
        if (function[i].type->getQualifier().storage != EvqConst)
            constType = false;
        if (function[i].type->isArray())
            arrayArg = true;
    }

    if (constType)
        type.getQualifier().storage = EvqConst;

    if (type.isArray()) {
        if (type.getArraySize() == 0) {
            // auto adapt the constructor type to the number of arguments
            type.changeArraySize(function.getParamCount());
        } else if (type.getArraySize() != function.getParamCount()) {
            error(loc, "array constructor needs one argument per array element", "constructor", "");
            return true;
        }
    }

    if (arrayArg && op != EOpConstructStruct) {
        error(loc, "constructing from a non-dereferenced array", "constructor", "");
        return true;
    }

    if (matrixInMatrix && ! type.isArray()) {
        profileRequires(loc, ENoProfile, 120, 0, "constructing matrix from matrix");
        return false;
    }

    if (overFull) {
        error(loc, "too many arguments", "constructor", "");
        return true;
    }

    if (op == EOpConstructStruct && ! type.isArray() && type.getStruct()->size() != function.getParamCount()) {
        error(loc, "Number of constructor parameters does not match the number of structure fields", "constructor", "");
        return true;
    }

    if ((op != EOpConstructStruct && size != 1 && size < type.getObjectSize()) ||
        (op == EOpConstructStruct && size < type.getObjectSize())) {
        error(loc, "not enough data provided for construction", "constructor", "");
        return true;
    }

    TIntermTyped* typed = node->getAsTyped();
    if (typed == 0) {
        error(loc, "constructor argument does not have a type", "constructor", "");
        return true;
    }
    if (op != EOpConstructStruct && typed->getBasicType() == EbtSampler) {
        error(loc, "cannot convert a sampler", "constructor", "");
        return true;
    }
    if (typed->getBasicType() == EbtVoid) {
        error(loc, "cannot convert a void", "constructor", "");
        return true;
    }

    return false;
}

// Checks to see if a void variable has been declared and raise an error message for such a case
//
// returns true in case of an error
//
bool TParseContext::voidErrorCheck(TSourceLoc loc, const TString& identifier, const TBasicType basicType)
{
    if (basicType == EbtVoid) {
        error(loc, "illegal use of type 'void'", identifier.c_str(), "");
        return true;
    }

    return false;
}

// Checks to see if the node (for the expression) contains a scalar boolean expression or not
void TParseContext::boolCheck(TSourceLoc loc, const TIntermTyped* type)
{
    if (type->getBasicType() != EbtBool || type->isArray() || type->isMatrix() || type->isVector())
        error(loc, "boolean expression expected", "", "");
}

// This function checks to see if the node (for the expression) contains a scalar boolean expression or not
void TParseContext::boolCheck(TSourceLoc loc, const TPublicType& pType)
{
    if (pType.basicType != EbtBool || pType.arraySizes || pType.matrixCols > 1 || (pType.vectorSize > 1))
        error(loc, "boolean expression expected", "", "");
}

bool TParseContext::samplerErrorCheck(TSourceLoc loc, const TPublicType& pType, const char* reason)
{
    if (pType.basicType == EbtStruct) {
        if (containsSampler(*pType.userDef)) {
            error(loc, reason, TType::getBasicString(pType.basicType), "(structure cannot contain a sampler or image)");

            return true;
        }

        return false;
    } else if (pType.basicType == EbtSampler) {
        error(loc, reason, TType::getBasicString(pType.basicType), "");

        return true;
    }

    return false;
}

//
// move from parameter/unknown qualifiers to pipeline in/out qualifiers
//
void TParseContext::pipeInOutFix(TSourceLoc loc, TQualifier& qualifier)
{
    switch (qualifier.storage) {
    case EvqIn:
        profileRequires(loc, ENoProfile, 130, 0, "in for stage inputs");
        profileRequires(loc, EEsProfile, 300, 0, "in for stage inputs");
        qualifier.storage = EvqVaryingIn;
        break;
    case EvqOut:
        profileRequires(loc, ENoProfile, 130, 0, "out for stage outputs");
        profileRequires(loc, EEsProfile, 300, 0, "out for stage outputs");
        qualifier.storage = EvqVaryingOut;
        break;
    case EvqInOut:
        qualifier.storage = EvqVaryingIn;
        error(loc, "cannot use 'inout' at global scope", "", "");
        break;
    default:
        break;
    }
}

void TParseContext::globalQualifierCheck(TSourceLoc loc, const TQualifier& qualifier, const TPublicType& publicType)
{
    if (! symbolTable.atGlobalLevel())
        return;

    // Do non-in/out error checks

    if (qualifier.storage != EvqUniform && samplerErrorCheck(loc, publicType, "samplers and images must be uniform"))
        return;

    if (qualifier.storage != EvqVaryingIn && qualifier.storage != EvqVaryingOut)
        return;

    // now, knowing it is a shader in/out, do all the in/out semantic checks

    if (publicType.basicType == EbtBool) {
        error(loc, "cannot be bool", GetStorageQualifierString(qualifier.storage), "");

        return;
    }

    if (language == EShLangVertex && qualifier.storage == EvqVaryingIn) {
        if (publicType.basicType == EbtStruct) {
            error(loc, "cannot be a structure or array", GetStorageQualifierString(qualifier.storage), "");

            return;
        }
        if (publicType.arraySizes) {
            requireProfile(loc, ~EEsProfile, "vertex input arrays");
            profileRequires(loc, ENoProfile, 150, 0, "vertex input arrays");
        }
    }

    if (language == EShLangFragment && qualifier.storage == EvqVaryingOut) {
        profileRequires(loc, EEsProfile, 300, 0, "fragment shader output");
        if (publicType.basicType == EbtStruct) {
            error(loc, "cannot be a structure", GetStorageQualifierString(qualifier.storage), "");

            return;
        }
    }

    if (publicType.basicType == EbtInt || publicType.basicType == EbtUint || publicType.basicType == EbtDouble) {
        profileRequires(loc, EEsProfile, 300, 0, "shader input/output");
        if (! qualifier.flat) {
            if (qualifier.storage == EvqVaryingIn && language == EShLangFragment)
                error(loc, "must be qualified as flat", TType::getBasicString(publicType.basicType), GetStorageQualifierString(qualifier.storage));
            else if (qualifier.storage == EvqVaryingOut && language == EShLangVertex && version == 300)
                error(loc, "must be qualified as flat", TType::getBasicString(publicType.basicType), GetStorageQualifierString(qualifier.storage));
        }
    }

    if (language == EShLangVertex && qualifier.storage == EvqVaryingIn &&
        (qualifier.isAuxiliary() || qualifier.isInterpolation() || qualifier.isMemory() || qualifier.invariant))
        error(loc, "vertex input cannot be further qualified", "", "");
}

//
// Merge characteristics of the 'src' qualifier into the 'dst'.
// If there is duplication, issue error messages, unless 'force'
// is specified, which means to just override default settings.
//
// Also, when force is false, it will be assumed that 'src' follows
// 'dst', for the purpose of error checking order for versions
// that require specific orderings of qualifiers.
//
void TParseContext::mergeQualifiers(TSourceLoc loc, TQualifier& dst, const TQualifier& src, bool force)
{
    // Multiple auxiliary qualifiers (mostly done later by 'individual qualifiers')
    if (src.isAuxiliary() && dst.isAuxiliary())
        error(loc, "can only have one auxiliary qualifier (centroid, patch, and sample)", "", "");

    // Multiple interpolation qualifiers (mostly done later by 'individual qualifiers')
    if (src.isInterpolation() && dst.isInterpolation())
        error(loc, "can only have one interpolation qualifier (flat, smooth, noperspective)", "", "");

    // Ordering
    if (! force && version < 420) {
        // non-function parameters
        if (src.invariant && (dst.isInterpolation() || dst.isAuxiliary() || dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "invariant qualifier must appear first", "", "");
        else if (src.isInterpolation() && (dst.isAuxiliary() || dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "interpolation qualifiers must appear before storage and precision qualifiers", "", "");
        else if (src.isAuxiliary() && (dst.storage != EvqTemporary || dst.precision != EpqNone))
            error(loc, "Auxiliary qualifiers (centroid, patch, and sample) must appear before storage and precision qualifiers", "", "");
        else if (src.storage != EvqTemporary && (dst.precision != EpqNone))
            error(loc, "precision qualifier must appear as last qualifier", "", "");

        // function parameters
        if (src.storage == EvqConst && (dst.storage == EvqIn || dst.storage == EvqOut))
            error(loc, "in/out must appear before const", "", "");
    }

    // Storage qualification
    if (dst.storage == EvqTemporary || dst.storage == EvqGlobal)
        dst.storage = src.storage;
    else if ((dst.storage == EvqIn  && src.storage == EvqOut) ||
             (dst.storage == EvqOut && src.storage == EvqIn))
        dst.storage = EvqInOut;
    else if ((dst.storage == EvqIn    && src.storage == EvqConst) ||
             (dst.storage == EvqConst && src.storage == EvqIn))
        dst.storage = EvqConstReadOnly;
    else if (src.storage != EvqTemporary)
        error(loc, "too many storage qualifiers", GetStorageQualifierString(src.storage), "");

    // Precision qualifiers
    if (! force && src.precision != EpqNone && dst.precision != EpqNone)
        error(loc, "only one precision qualifier allowed", GetPrecisionQualifierString(src.precision), "");
    if (dst.precision == EpqNone || (force && src.precision != EpqNone))
        dst.precision = src.precision;

    // Layout qualifiers
    mergeLayoutQualifiers(loc, dst, src);

    // individual qualifiers
    bool repeated = false;
    #define MERGE_SINGLETON(field) repeated |= dst.field && src.field; dst.field |= src.field;
    MERGE_SINGLETON(invariant);
    MERGE_SINGLETON(centroid);
    MERGE_SINGLETON(smooth);
    MERGE_SINGLETON(flat);
    MERGE_SINGLETON(nopersp);
    MERGE_SINGLETON(patch);
    MERGE_SINGLETON(sample);
    MERGE_SINGLETON(shared);
    MERGE_SINGLETON(coherent);
    MERGE_SINGLETON(volatil);
    MERGE_SINGLETON(restrict);
    MERGE_SINGLETON(readonly);
    MERGE_SINGLETON(writeonly);

    if (repeated)
        error(loc, "replicated qualifiers", "", "");
}

void TParseContext::setDefaultPrecision(TSourceLoc loc, TPublicType& publicType, TPrecisionQualifier qualifier)
{
    TBasicType basicType = publicType.basicType;

    if (basicType == EbtSampler) {
        defaultSamplerPrecision[computeSamplerTypeIndex(publicType.sampler)] = qualifier;

        return;  // all is well
    }

    if (basicType == EbtInt || basicType == EbtFloat) {
        if (publicType.isScalar()) {
            defaultPrecision[basicType] = qualifier;
            if (basicType == EbtInt)
                defaultPrecision[EbtUint] = qualifier;

            return;  // all is well
        }
    }

    error(loc, "cannot apply precision statement to this type; use 'float', 'int' or a sampler type", TType::getBasicString(basicType), "");
}

// used to flatten the sampler type space into a single dimension
// correlates with the declaration of defaultSamplerPrecision[]
int TParseContext::computeSamplerTypeIndex(TSampler& sampler)
{
    int arrayIndex = sampler.arrayed ? 1 : 0;
    int shadowIndex = sampler.shadow ? 1 : 0;

    return EsdNumDims * (EbtNumTypes * (2 * arrayIndex + shadowIndex) + sampler.type) + sampler.dim;
}

TPrecisionQualifier TParseContext::getDefaultPrecision(TPublicType& publicType)
{
    if (publicType.basicType == EbtSampler)
        return defaultSamplerPrecision[computeSamplerTypeIndex(publicType.sampler)];
    else
        return defaultPrecision[publicType.basicType];
}

void TParseContext::precisionQualifierCheck(TSourceLoc loc, TPublicType& publicType)
{
    // Built-in symbols are allowed some ambiguous precisions, to be pinned down
    // later by context.
    if (profile != EEsProfile || parsingBuiltins)
        return;

    if (publicType.basicType == EbtFloat || publicType.basicType == EbtUint || publicType.basicType == EbtInt || publicType.basicType == EbtSampler) {
        if (publicType.qualifier.precision == EpqNone) {
            if (messages & EShMsgRelaxedErrors)
                warn(loc, "type requires declaration of default precision qualifier", TType::getBasicString(publicType.basicType), "substituting 'mediump'");
            else
                error(loc, "type requires declaration of default precision qualifier", TType::getBasicString(publicType.basicType), "");
            publicType.qualifier.precision = EpqMedium;
            defaultPrecision[publicType.basicType] = EpqMedium;
        }
    } else if (publicType.qualifier.precision != EpqNone)
        error(loc, "type cannot have precision qualifier", TType::getBasicString(publicType.basicType), "");
}

void TParseContext::parameterSamplerCheck(TSourceLoc loc, TStorageQualifier qualifier, const TType& type)
{
    if ((qualifier == EvqOut || qualifier == EvqInOut) && type.getBasicType() != EbtStruct && type.getBasicType() == EbtSampler)
        error(loc, "samplers cannot be output parameters", type.getCompleteTypeString().c_str(), "");
}

bool TParseContext::containsSampler(const TType& type)
{
    if (type.getBasicType() == EbtSampler)
        return true;

    if (type.getBasicType() == EbtStruct) {
        TTypeList& structure = *type.getStruct();
        for (unsigned int i = 0; i < structure.size(); ++i) {
            if (containsSampler(*structure[i].type))
                return true;
        }
    }

    return false;
}

//
// Do size checking for an array type's size.
//
void TParseContext::arraySizeCheck(TSourceLoc loc, TIntermTyped* expr, int& size)
{
    TIntermConstantUnion* constant = expr->getAsConstantUnion();
    if (constant == 0 || (constant->getBasicType() != EbtInt && constant->getBasicType() != EbtUint)) {
        error(loc, "array size must be a constant integer expression", "", "");
        size = 1;

        return;
    }

    size = constant->getConstArray()[0].getIConst();

    if (size <= 0) {
        error(loc, "array size must be a positive integer", "", "");
        size = 1;

        return;
    }
}

//
// See if this qualifier can be an array.
//
// Returns true if there is an error.
//
bool TParseContext::arrayQualifierError(TSourceLoc loc, const TQualifier& qualifier)
{
    if (qualifier.storage == EvqConst) {
        profileRequires(loc, ENoProfile, 120, GL_3DL_array_objects, "const array");
        profileRequires(loc, EEsProfile, 300, 0, "const array");
    }

    if (qualifier.storage == EvqVaryingIn && language == EShLangVertex) {
        requireProfile(loc, ~EEsProfile, "vertex input arrays");
        profileRequires(loc, ENoProfile, 150, 0, "vertex input arrays");
    }

    return false;
}

//
// Require array to have size
//
void TParseContext::arraySizeRequiredCheck(TSourceLoc loc, int size)
{
    if (size == 0) {
        error(loc, "array size required", "", "");
        size = 1;
    }
}

void TParseContext::arrayDimError(TSourceLoc loc)
{
    requireProfile(loc, ECoreProfile | ECompatibilityProfile, "arrays of arrays");
    profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, 0, "arrays of arrays");
}

void TParseContext::arrayDimCheck(TSourceLoc loc, TArraySizes* sizes1, TArraySizes* sizes2)
{
    if ((sizes1 && sizes2) ||
        (sizes1 && sizes1->isArrayOfArrays()) ||
        (sizes2 && sizes2->isArrayOfArrays()))
        arrayDimError(loc);
}

void TParseContext::arrayDimCheck(TSourceLoc loc, const TType* type, TArraySizes* sizes2)
{
    if ((type && type->isArray() && sizes2) ||
        (sizes2 && sizes2->isArrayOfArrays()))
        arrayDimError(loc);
}

//
// Do all the semantic checking for declaring an array, with and
// without a size, and make the right changes to the symbol table.
//
// size == 0 means no specified size.
//
void TParseContext::declareArray(TSourceLoc loc, TString& identifier, const TType& type, TSymbol*& symbol, bool& newDeclaration)
{
    if (! symbol) {
        bool currentScope;
        symbol = symbolTable.find(identifier, 0, &currentScope);
        if (symbol == 0 || ! currentScope) {
            //
            // Successfully process a new definition.
            // (Redeclarations have to take place at the same scope; otherwise they are hiding declarations)
            //
            symbol = new TVariable(&identifier, type);
            symbolTable.insert(*symbol);
            newDeclaration = true;
            return;
        }
        if (symbol->getAsAnonMember()) {
            error(loc, "cannot redeclare a user-block member array", identifier.c_str(), "");
            return;
        }
    }

    //
    // Process a redeclaration.
    //

    if (! symbol) {
        error(loc, "array variable name expected", identifier.c_str(), "");
        return;
    }

    TType& newType = symbol->getWritableType();

    if (! newType.isArray()) {
        error(loc, "redeclaring non-array as array", identifier.c_str(), "");
        return;
    }
    if (newType.getArraySize() > 0) {
        error(loc, "redeclaration of array with size", identifier.c_str(), "");
        return;
    }

    if (! newType.sameElementType(type)) {
        error(loc, "redeclaration of array with a different newType", identifier.c_str(), "");
        return;
    }

    newType.shareArraySizes(type);
}

void TParseContext::updateMaxArraySize(TSourceLoc loc, TIntermNode *node, int index)
{
    TIntermSymbol* symbolNode = node->getAsSymbolNode();
    if (! symbolNode) {
        // TODO: functionality: unsized arrays: handle members of blocks
        return;
    }

    // maybe there is nothing to do...
    // TODO: functionality: unsized arrays: is the node sharing the array type with the symbol table?
    if (symbolNode->getType().getMaxArraySize() > index)
        return;

    // something to do...

    TSymbol* symbol = symbolTable.find(symbolNode->getName());
    assert(symbol);
    if (symbol == 0)
        return;

    if (symbol->getAsFunction()) {
        error(loc, "array variable name expected", symbolNode->getName().c_str(), "");
        return;
    }

    // For read-only built-ins, add a new variable for holding the maximum array size of an implicitly-sized shared array.
    // TODO: functionality: unsized arrays: is this new array type shared with the node?
    if (symbol->isReadOnly())
        symbol = symbolTable.copyUp(symbol);

    symbol->getWritableType().setMaxArraySize(index + 1);
}

//
// Enforce non-initializer type/qualifier rules.
//
void TParseContext::nonInitConstCheck(TSourceLoc loc, TString& identifier, TType& type)
{
    //
    // Make the qualifier make sense, given that there is an initializer.
    //
    if (type.getQualifier().storage == EvqConst || 
        type.getQualifier().storage == EvqConstReadOnly) {
        type.getQualifier().storage = EvqTemporary;
        error(loc, "variables with qualifier 'const' must be initialized", identifier.c_str(), "");
    }
}

//
// See if the identifier is a built-in symbol that can be redeclared, and if so,
// copy the symbol table's read-only built-in variable to the current
// global level, where it can be modified based on the passed in type.
//
// Returns 0 if no redeclaration took place; meaning a normal declaration still
// needs to occur for it, not necessarily an error.
//
// Returns a redeclared and type-modified variable if a redeclarated occurred.
//
TSymbol* TParseContext::redeclareBuiltin(TSourceLoc loc, const TString& identifier, bool& newDeclaration)
{
    if (profile == EEsProfile || identifier.compare(0, 3, "gl_") != 0 || symbolTable.atBuiltInLevel())
        return 0;

    // Potentially redeclaring a built-in variable...

    if ((identifier == "gl_FragDepth"           && version >= 420) ||
        (identifier == "gl_PerVertex"           && version >= 410) ||
        (identifier == "gl_PerFragment"         && version >= 410) ||
        (identifier == "gl_FragCoord"           && version >= 150) ||
        (identifier == "gl_ClipDistance"        && version >= 130) ||
        (identifier == "gl_FrontColor"          && version >= 130) ||
        (identifier == "gl_BackColor"           && version >= 130) ||
        (identifier == "gl_FrontSecondaryColor" && version >= 130) ||
        (identifier == "gl_BackSecondaryColor"  && version >= 130) ||
        (identifier == "gl_SecondaryColor"      && version >= 130) ||
        (identifier == "gl_Color"               && version >= 130 && language == EShLangFragment) ||
         identifier == "gl_TexCoord") {

        // Find the existing symbol, if any.
        bool builtIn;
        TSymbol* symbol = symbolTable.find(identifier, &builtIn);

        // If the symbol was not found, this must be a version/profile/stage
        // that doesn't have it.
        if (! symbol)
            return 0;

        // If it wasn't at a built-in level, then it's already been redeclared;
        // that is, this is a redeclaration of a redeclaration, reuse that initial
        // redeclaration.  Otherwise, make the new one.
        if (builtIn) {
            // Copy the symbol up to make a writable version
            newDeclaration = true;
            symbol = symbolTable.copyUp(symbol);
        }

        // Now, modify the type of the copy, as per the type of the current redeclaration.
        // TODO: functionality: verify type change is allowed and make the change in type

        return symbol;
    }

    return 0;
}

void TParseContext::paramCheck(TSourceLoc loc, const TStorageQualifier& qualifier, TType* type)
{
    switch (qualifier) {
    case EvqConst:
    case EvqConstReadOnly:
        type->getQualifier().storage = EvqConstReadOnly;
        break;
    case EvqIn:
    case EvqOut:
    case EvqInOut:
        type->getQualifier().storage = qualifier;
        break;
    case EvqTemporary:
        type->getQualifier().storage = EvqIn;
        break;
    default:
        type->getQualifier().storage = EvqIn;
        error(loc, "qualifier not allowed on function parameter", GetStorageQualifierString(qualifier), "");
        break;
    }
}

void TParseContext::nestedBlockCheck(TSourceLoc loc)
{
    if (structNestingLevel > 0)
        error(loc, "cannot nest a block definition inside a structure or block", "", "");
    ++structNestingLevel;
}

void TParseContext::nestedStructCheck(TSourceLoc loc)
{
    if (structNestingLevel > 0)
        error(loc, "cannot nest a structure definition inside a structure or block", "", "");
    ++structNestingLevel;
}

void TParseContext::arrayObjectCheck(TSourceLoc loc, const TType& type, const char* op)
{
    // Some versions don't allow comparing arrays or structures containing arrays
    if (type.containsArray()) {
        profileRequires(loc, ENoProfile, 120, GL_3DL_array_objects, op);
        profileRequires(loc, EEsProfile, 300, 0, op);
    }
}

//
// See if this loop satisfies the limitations for ES 2.0 (version 100) for loops in Appendex A:
//
// "The loop index has type int or float.
//
// "The for statement has the form:
//     for ( init-declaration ; condition ; expression ) 
//     init-declaration has the form: type-specifier identifier = constant-expression
//     condition has the form:  loop-index relational_operator constant-expression
//         where relational_operator is one of: > >= < <= == or !=
//     expression [sic] has one of the following forms:
//         loop-index++
//         loop-index--
//         loop-index += constant-expression
//         loop-index -= constant-expression
//
// The body is handled in an AST traversal.
//
void TParseContext::inductiveLoopCheck(TSourceLoc loc, TIntermNode* init, TIntermLoop* loop)
{
    // loop index init must exist and be a declaration, which shows up in the AST as an aggregate of size 1 of the declaration
    bool badInit = false;
    if (! init || ! init->getAsAggregate() || ! init->getAsAggregate()->getSequence().size() == 1)
        badInit = true;
    TIntermBinary* binaryInit;
    if (! badInit) {
        // get the declaration assignment
        binaryInit = init->getAsAggregate()->getSequence()[0]->getAsBinaryNode();
        if (! binaryInit)
            badInit = true;
    }
    if (badInit) {
        error(loc, "inductive-loop init-declaration requires the form \"type-specifier loop-index = constant-expression\"", "limitations", "");
        return;
    }

    // loop index must be type int or float
    if (! binaryInit->getType().isScalar() || (binaryInit->getBasicType() != EbtInt && binaryInit->getBasicType() != EbtFloat)) {
        error(loc, "inductive loop requires a scalar 'int' or 'float' loop index", "limitations", "");
        return;
    }

    // init is the form "loop-index = constant"
    if (binaryInit->getOp() != EOpAssign || ! binaryInit->getLeft()->getAsSymbolNode() || ! binaryInit->getRight()->getAsConstantUnion()) {
        error(loc, "inductive-loop init-declaration requires the form \"type-specifier loop-index = constant-expression\"", "limitations", "");
        return;
    }
    
    // get the unique id of the loop index
    int loopIndex = binaryInit->getLeft()->getAsSymbolNode()->getId();
    inductiveLoopIds.insert(loopIndex);
    
    // condition's form must be "loop-index relational-operator constant-expression"
    bool badCond = ! loop->getTest();
    if (! badCond) {
        TIntermBinary* binaryCond = loop->getTest()->getAsBinaryNode();
        badCond = ! binaryCond;
        if (! badCond) {
            switch (binaryCond->getOp()) {
            case EOpGreaterThan:
            case EOpGreaterThanEqual:
            case EOpLessThan:
            case EOpLessThanEqual:
            case EOpEqual:
            case EOpNotEqual:
                break;
            default:
                badCond = true;
            }
        }
        if (binaryCond && (! binaryCond->getLeft()->getAsSymbolNode() ||
                           binaryCond->getLeft()->getAsSymbolNode()->getId() != loopIndex ||
                           ! binaryCond->getRight()->getAsConstantUnion()))
            badCond = true;
    }
    if (badCond) {
        error(loc, "inductive-loop condition requires the form \"loop-index <comparison-op> constant-expression\"", "limitations", "");
        return;
    }

    // loop-index++
    // loop-index--
    // loop-index += constant-expression
    // loop-index -= constant-expression
    bool badTerminal = ! loop->getTerminal();
    if (! badTerminal) {
        TIntermUnary* unaryTerminal = loop->getTerminal()->getAsUnaryNode();
        TIntermBinary* binaryTerminal = loop->getTerminal()->getAsBinaryNode();
        if (unaryTerminal || binaryTerminal) {
            switch(loop->getTerminal()->getAsOperator()->getOp()) {
            case EOpPostDecrement:
            case EOpPostIncrement:
            case EOpAddAssign:
            case EOpSubAssign:
                break;
            default:
                badTerminal = true;
            }
        } else
            badTerminal = true;
        if (binaryTerminal && (! binaryTerminal->getLeft()->getAsSymbolNode() ||
                               binaryTerminal->getLeft()->getAsSymbolNode()->getId() != loopIndex ||
                               ! binaryTerminal->getRight()->getAsConstantUnion()))
            badTerminal = true;
        if (unaryTerminal && (! unaryTerminal->getOperand()->getAsSymbolNode() ||
                              unaryTerminal->getOperand()->getAsSymbolNode()->getId() != loopIndex))
            badTerminal = true;
    }
    if (badTerminal) {
        error(loc, "inductive-loop termination requires the form \"loop-index++, loop-index--, loop-index += constant-expression, or loop-index -= constant-expression\"", "limitations", "");
        return;
    }

    // the body
    inductiveLoopBodyCheck(loop->getBody(), loopIndex, symbolTable);
}

//
// Do any additional error checking, etc., once we know the parsing is done.
//
void TParseContext::finalize()
{
    // Check on array indexes for ES 2.0 (version 100) limitations.
    for (size_t i = 0; i < needsIndexLimitationChecking.size(); ++i)
        constantIndexExpressionCheck(needsIndexLimitationChecking[i]);
}

//
// Layout qualifier stuff.
//

// Put the id's layout qualification into the public type.  This is before we know any
// type information for error checking.
void TParseContext::setLayoutQualifier(TSourceLoc loc, TPublicType& publicType, TString& id)
{
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    if (id == TQualifier::getLayoutMatrixString(ElmColumnMajor))
        publicType.qualifier.layoutMatrix = ElmColumnMajor;
    else if (id == TQualifier::getLayoutMatrixString(ElmRowMajor))
        publicType.qualifier.layoutMatrix = ElmRowMajor;
    else if (id == TQualifier::getLayoutPackingString(ElpPacked))
        publicType.qualifier.layoutPacking = ElpPacked;
    else if (id == TQualifier::getLayoutPackingString(ElpShared))
        publicType.qualifier.layoutPacking = ElpShared;
    else if (id == TQualifier::getLayoutPackingString(ElpStd140))
        publicType.qualifier.layoutPacking = ElpStd140;
    else if (id == TQualifier::getLayoutPackingString(ElpStd430)) {
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "std430");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, 0, "std430");
        publicType.qualifier.layoutPacking = ElpStd430;
    } else if (id == "location")
        error(loc, "requires an integer assignment (e.g., location = 4)", "location", "");
    else if (id == "binding") {
        error(loc, "requires an integer assignment (e.g., binding = 4)", "binding", "");
    } else
        error(loc, "unrecognized layout identifier", id.c_str(), "");
}

// Put the id's layout qualifier value into the public type.  This is before we know any
// type information for error checking.
void TParseContext::setLayoutQualifier(TSourceLoc loc, TPublicType& publicType, TString& id, int value)
{
    std::transform(id.begin(), id.end(), id.begin(), ::tolower);
    if (id == "location") {
        requireProfile(loc, EEsProfile | ECoreProfile | ECompatibilityProfile, "location");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 330, 0, "location");
        if ((unsigned int)value >= TQualifier::layoutLocationEnd)
            error(loc, "location is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutSlotLocation = value;
    } else if (id == "binding") {
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "binding");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 420, GL_ARB_shading_language_420pack, "binding");
        if ((unsigned int)value >= TQualifier::layoutBindingEnd)
            error(loc, "binding is too large", id.c_str(), "");
        else
            publicType.qualifier.layoutBinding = value;
    } else
        error(loc, "there is no such layout identifier taking an assigned value", id.c_str(), "");

    // TODO: semantics: error check: make sure locations are non-overlapping across the whole stage
    // TODO: semantics: error check: output arrays can only be indexed with a constant (es 300)
}

// Merge any layout qualifier information from src into dst, leaving everything else in dst alone
void TParseContext::mergeLayoutQualifiers(TSourceLoc loc, TQualifier& dst, const TQualifier& src)
{
    if (src.layoutMatrix != ElmNone)
        dst.layoutMatrix = src.layoutMatrix;

    if (src.layoutPacking != ElpNone)
        dst.layoutPacking = src.layoutPacking;

    if (src.hasLocation())
        dst.layoutSlotLocation = src.layoutSlotLocation;

    if (src.hasBinding())
        dst.layoutBinding = src.layoutBinding;
}

// Do error layout error checking given a full variable/block declaration.
void TParseContext::layoutCheck(TSourceLoc loc, const TSymbol& symbol)
{
    const TType& type = symbol.getType();
    const TQualifier& qualifier = type.getQualifier();
    
    if (qualifier.hasLocation()) {
        switch (qualifier.storage) {
        case EvqVaryingIn:
        {
            const char* feature = "location qualifier on input";
            if (profile == EEsProfile)
                requireStage(loc, EShLangVertex, feature);
            requireStage(loc, (EShLanguageMask)~EShLangComputeMask, feature);
            if (language == EShLangVertex)
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 330, 0, feature);
            else
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 410, GL_ARB_separate_shader_objects, feature);
            if (type.getBasicType() == EbtBlock)
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, 0 /* TODO ARB_enhanced_layouts*/, "location qualifier on input block");
            break;
        }
        case EvqVaryingOut:
        {
            const char* feature = "location qualifier on output";
            if (profile == EEsProfile)
                requireStage(loc, EShLangFragment, feature);
            requireStage(loc, (EShLanguageMask)~EShLangComputeMask, feature);
            if (language == EShLangFragment)
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 330, 0, feature);
            else
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 410, GL_ARB_separate_shader_objects, feature);
            if (type.getBasicType() == EbtBlock)
                profileRequires(loc, ECoreProfile | ECompatibilityProfile, 440, 0 /* TODO ARB_enhanced_layouts*/, "location qualifier on output block");
            break;
        }
        case EvqUniform:
        case EvqBuffer:
        {
            const char* feature = "location qualifier on uniform or buffer";
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, feature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, 0, feature);
            if (symbol.getAsVariable() == 0)
                error(loc, "can only be used on variable declaration", feature, "");
            break;
        }
        default:
            break;
        }
    }

    if (qualifier.hasBinding()) {
        // Binding checking, from the spec:
        //
        // "If the binding point for any uniform or shader storage block instance is less than zero, or greater than or 
        // equal to the implementation-dependent maximum number of uniform buffer bindings, a compile-time 
        // error will occur. When the binding identifier is used with a uniform or shader storage block instanced as 
        // an array of size N, all elements of the array from binding through binding + N � 1 must be within this 
        // range."
        //
        // TODO:  binding error checking against limits, arrays
        //
        if (qualifier.storage != EvqUniform && qualifier.storage != EvqBuffer)
            error(loc, "requires uniform or buffer storage qualifier", "binding", "");
        if (type.getBasicType() != EbtSampler && type.getBasicType() != EbtBlock)
            error(loc, "requires block, or sampler/image, or atomic-counter type", "binding", "");
            // TODO: atomic counter functionality: include in test above
    }
}

/////////////////////////////////////////////////////////////////////////////////
//
// Non-Errors.
//
/////////////////////////////////////////////////////////////////////////////////

//
// Look up a function name in the symbol table, and make sure it is a function.
//
// Return the function symbol if found, otherwise 0.
//
const TFunction* TParseContext::findFunction(TSourceLoc loc, TFunction* call, bool *builtIn)
{
    TSymbol* symbol = symbolTable.find(call->getMangledName(), builtIn);

    if (symbol == 0) {
        error(loc, "no matching overloaded function found", call->getName().c_str(), "");

        return 0;
    }

    const TFunction* function = symbol->getAsFunction();
    if (! function) {
        error(loc, "function name expected", call->getName().c_str(), "");

        return 0;
    }

    return function;
}

//
// Do everything necessary to handle a variable (non-block) declaration.
// Either redeclaring a variable, or making a new one, updating the symbol
// table, and all error checking.
//
// Returns a subtree node that computes an initializer, if needed.
// Returns 0 if there is no code to execute for initialization.
//
TIntermNode* TParseContext::declareVariable(TSourceLoc loc, TString& identifier, TPublicType& publicType, TArraySizes* arraySizes, TIntermTyped* initializer)
{
    TType type(publicType);

    if (voidErrorCheck(loc, identifier, type.getBasicType()))
        return 0;

    if (! initializer)
        nonInitConstCheck(loc, identifier, type);

    // Check for redeclaration of built-ins and/or attempting to declare a reserved name
    bool newDeclaration = false;    // true if a new entry gets added to the symbol table
    TSymbol* symbol = redeclareBuiltin(loc, identifier, newDeclaration);
    if (! symbol)
        reservedErrorCheck(loc, identifier);

    // Declare the variable
    if (arraySizes) {
        // for ES, since size isn't coming from an initializer, it has to be explicitly declared now
        if (profile == EEsProfile && ! initializer)
            arraySizeRequiredCheck(loc, arraySizes->getSize());

        arrayDimCheck(loc, &type, arraySizes);
        if (! arrayQualifierError(loc, type.getQualifier())) {
            type.setArraySizes(arraySizes);
            declareArray(loc, identifier, type, symbol, newDeclaration);
        }

        if (initializer) {
            profileRequires(loc, ENoProfile, 120, GL_3DL_array_objects, "initializer");
            profileRequires(loc, EEsProfile, 300, 0, "initializer");
        }
    } else {
        // non-array case
        if (! symbol)
            symbol = declareNonArray(loc, identifier, type, newDeclaration);
    }

    // Deal with initializer
    TIntermNode* initNode = 0;
    if (symbol && initializer) {
        TVariable* variable = symbol->getAsVariable();
        if (! variable) {
            error(loc, "initializer requires a variable, not a member", identifier.c_str(), "");
            return 0;
        }
        initNode = executeInitializer(loc, identifier, initializer, variable);
    }

    // look for errors in layout qualifier use
    if (symbol)
        layoutCheck(loc, *symbol);

    // see if it's a linker-level object to track
    if (symbol && newDeclaration && symbolTable.atGlobalLevel())
        intermediate.addSymbolLinkageNode(linkage, *symbol);

    return initNode;
}

//
// Declare a non-array variable, the main point being there is no redeclaration
// for resizing allowed.
//
// Return the successfully declared variable.
//
TVariable* TParseContext::declareNonArray(TSourceLoc loc, TString& identifier, TType& type, bool& newDeclaration)
{
    // make a new variable
    TVariable* variable = new TVariable(&identifier, type);

    // add variable to symbol table
    if (! symbolTable.insert(*variable)) {
        error(loc, "redefinition", variable->getName().c_str(), "");
        return 0;
    } else {
        newDeclaration = true;
        return variable;
    }
}

//
// Handle all types of initializers from the grammar.
//
// Returning 0 just means there is no code to execute to handle the
// initializer, which will, for example, be the case for constant initalizers.
//
TIntermNode* TParseContext::executeInitializer(TSourceLoc loc, TString& identifier,
                                               TIntermTyped* initializer, TVariable* variable)
{
    //
    // Identifier must be of type constant, a global, or a temporary, and
    // starting at version 120, desktop allows uniforms to have initializers.
    //
    TStorageQualifier qualifier = variable->getType().getQualifier().storage;
    if (! (qualifier == EvqTemporary || qualifier == EvqGlobal || qualifier == EvqConst ||
           (qualifier == EvqUniform && profile != EEsProfile && version >= 120))) {
        error(loc, " cannot initialize this type of qualifier ", variable->getType().getStorageQualifierString(), "");
        return 0;
    }

    //
    // If the initializer was from braces { ... }, we convert the whole subtree to a
    // constructor-style subtree, allowing the rest of the code to operate
    // identically for both kinds of initializers.
    //
    initializer = convertInitializerList(loc, variable->getType(), initializer);
    if (! initializer) {
        // error recovery; don't leave const without constant values
        if (qualifier == EvqConst)            
            variable->getWritableType().getQualifier().storage = EvqTemporary;
        return 0;
    }

    // Fix arrayness if variable is unsized, getting size from the initializer
    if (initializer->getType().isArray() && initializer->getType().getArraySize() > 0 &&
        variable->getType().isArray() && variable->getType().getArraySize() == 0)
        variable->getWritableType().changeArraySize(initializer->getType().getArraySize());

    // Uniform and global consts require a constant initializer
    if (qualifier == EvqUniform && initializer->getType().getQualifier().storage != EvqConst) {
        error(loc, "uniform initializers must be constant", "=", "'%s'", variable->getType().getCompleteString().c_str());
        variable->getWritableType().getQualifier().storage = EvqTemporary;
        return 0;
    }
    if (qualifier == EvqConst && symbolTable.atGlobalLevel() && initializer->getType().getQualifier().storage != EvqConst) {
        error(loc, "global const initializers must be constant", "=", "'%s'", variable->getType().getCompleteString().c_str());
        variable->getWritableType().getQualifier().storage = EvqTemporary;
        return 0;
    }

    // Const variables require a constant initializer, depending on version
    if (qualifier == EvqConst) {
        if (initializer->getType().getQualifier().storage != EvqConst) {
            const char* initFeature = "non-constant initializer";
            requireProfile(loc, ECoreProfile | ECompatibilityProfile, initFeature);
            profileRequires(loc, ECoreProfile | ECompatibilityProfile, 420, GL_ARB_shading_language_420pack, initFeature);
            variable->getWritableType().getQualifier().storage = EvqConstReadOnly;
            qualifier = EvqConstReadOnly;
        }
    }

    if (qualifier == EvqConst || qualifier == EvqUniform) {
        // Compile-time tagging of the variable with it's constant value...

        initializer = intermediate.addConversion(EOpAssign, variable->getType(), initializer);
        if (! initializer || ! initializer->getAsConstantUnion() || variable->getType() != initializer->getType()) {
            error(loc, "non-matching or non-convertible constant type for const initializer",
                  variable->getType().getStorageQualifierString(), "");
            variable->getWritableType().getQualifier().storage = EvqTemporary;
            return 0;
        }

        variable->setConstArray(initializer->getAsConstantUnion()->getConstArray());
    } else {
        // normal assigning of a value to a variable...
        TIntermSymbol* intermSymbol = intermediate.addSymbol(variable->getUniqueId(), variable->getName(), variable->getType(), loc);
        TIntermNode* initNode = intermediate.addAssign(EOpAssign, intermSymbol, initializer, loc);
        if (! initNode)
            assignError(loc, "=", intermSymbol->getCompleteString(), initializer->getCompleteString());

        return initNode;
    }

    return 0;
}

//
// Reprocess any initalizer-list { ... } parts of the initializer.
// Need to heirarchically assign correct types and implicit
// conversions. Will do this mimicking the same process used for
// creating a constructor-style initializer, ensuring we get the
// same form.
//
TIntermTyped* TParseContext::convertInitializerList(TSourceLoc loc, const TType& type, TIntermTyped* initializer)
{
    // Will operate recursively.  Once a subtree is found that is constructor style,
    // everything below it is already good: Only the "top part" of the initializer
    // can be an initializer list, where "top part" can extend for several (or all) levels.
    
    // see if we have bottomed out in the tree within the initializer-list part
    TIntermAggregate* initList = initializer->getAsAggregate();
    if (! initList || initList->getOp() != EOpNull)
        return initializer;

    // Of the initializer-list set of nodes, need to process bottom up, 
    // so recurse deep, then process on the way up.

    // Go down the tree here...
    if (type.isArray()) {
        // The type's array might be unsized, which could be okay, so base sizes on the size of the aggregate.
        // Later on, initializer execution code will deal with array size logic.
        TType arrayType;
        arrayType.shallowCopy(type);
        arrayType.setArraySizes(type);
        arrayType.changeArraySize(initList->getSequence().size());
        TType elementType;
        elementType.shallowCopy(arrayType);  // TODO: arrays of arrays: combine this with deref.
        elementType.dereference();
        for (size_t i = 0; i < initList->getSequence().size(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, elementType, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == 0)
                return 0;
        }

        return addConstructor(loc, initList, arrayType, mapTypeToConstructorOp(arrayType));
    } else if (type.getStruct()) {
        if (type.getStruct()->size() != initList->getSequence().size()) {
            error(loc, "wrong number of structure members", "initializer list", "");
            return 0;
        }
        for (size_t i = 0; i < type.getStruct()->size(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, *(*type.getStruct())[i].type, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == 0)
                return 0;
        }
    } else if (type.isMatrix()) {
        if (type.getMatrixCols() != initList->getSequence().size()) {
            error(loc, "wrong number of matrix columns:", "initializer list", type.getCompleteString().c_str());
            return 0;
        }
        TType vectorType;
        vectorType.shallowCopy(type);  // TODO: arrays of arrays: combine this with deref.
        vectorType.dereference();
        for (int i = 0; i < type.getMatrixCols(); ++i) {
            initList->getSequence()[i] = convertInitializerList(loc, vectorType, initList->getSequence()[i]->getAsTyped());
            if (initList->getSequence()[i] == 0)
                return 0;
        }
    } else if (type.isVector()) {
        if (type.getVectorSize() != initList->getSequence().size()) {
            error(loc, "wrong vector size (or rows in a matrix column):", "initializer list", type.getCompleteString().c_str());
            return 0;
        }
    } else {
        error(loc, "unexpected initializer-list type:", "initializer list", type.getCompleteString().c_str());
        return 0;
    }

    // now that the subtree is processed, process this node
    return addConstructor(loc, initList, type, mapTypeToConstructorOp(type));
}

//
// Test for the correctness of the parameters passed to various constructor functions
// and also convert them to the right data type, if allowed and required.
//
// Returns 0 for an error or the constructed node (aggregate or typed) for no error.
//
TIntermTyped* TParseContext::addConstructor(TSourceLoc loc, TIntermNode* node, const TType& type, TOperator op)
{
    if (node == 0)
        return 0;

    TIntermAggregate* aggrNode = node->getAsAggregate();

    TTypeList::iterator memberTypes;
    if (op == EOpConstructStruct)
        memberTypes = type.getStruct()->begin();

    TType elementType;
    elementType.shallowCopy(type);
    if (type.isArray())
        elementType.dereference();    // TODO: arrays of arrays: combine this with shallowCopy

    bool singleArg;
    if (aggrNode) {
        if (aggrNode->getOp() != EOpNull || aggrNode->getSequence().size() == 1)
            singleArg = true;
        else
            singleArg = false;
    } else
        singleArg = true;

    TIntermTyped *newNode;
    if (singleArg) {
        // If structure constructor or array constructor is being called
        // for only one parameter inside the structure, we need to call constructStruct function once.
        if (type.isArray())
            newNode = constructStruct(node, elementType, 1, node->getLoc());
        else if (op == EOpConstructStruct)
            newNode = constructStruct(node, *(*memberTypes).type, 1, node->getLoc());
        else
            newNode = constructBuiltIn(type, op, node, node->getLoc(), false);

        if (newNode && (type.isArray() || op == EOpConstructStruct))
            newNode = intermediate.setAggregateOperator(newNode, EOpConstructStruct, type, loc);

        return newNode;
    }

    //
    // Handle list of arguments.
    //
    TIntermSequence &sequenceVector = aggrNode->getSequence();    // Stores the information about the parameter to the constructor
    // if the structure constructor contains more than one parameter, then construct
    // each parameter

    int paramCount = 0;  // keeps a track of the constructor parameter number being checked

    // for each parameter to the constructor call, check to see if the right type is passed or convert them
    // to the right type if possible (and allowed).
    // for structure constructors, just check if the right type is passed, no conversion is allowed.

    for (TIntermSequence::iterator p = sequenceVector.begin();
                                   p != sequenceVector.end(); p++, paramCount++) {
        if (type.isArray())
            newNode = constructStruct(*p, elementType, paramCount+1, node->getLoc());
        else if (op == EOpConstructStruct)
            newNode = constructStruct(*p, *(memberTypes[paramCount]).type, paramCount+1, node->getLoc());
        else
            newNode = constructBuiltIn(type, op, *p, node->getLoc(), true);

        if (newNode)
            *p = newNode;
        else
            return 0;
    }

    TIntermTyped* constructor = intermediate.setAggregateOperator(aggrNode, op, type, loc);

    return constructor;
}

// Function for constructor implementation. Calls addUnaryMath with appropriate EOp value
// for the parameter to the constructor (passed to this function). Essentially, it converts
// the parameter types correctly. If a constructor expects an int (like ivec2) and is passed a
// float, then float is converted to int.
//
// Returns 0 for an error or the constructed node.
//
TIntermTyped* TParseContext::constructBuiltIn(const TType& type, TOperator op, TIntermNode* node, TSourceLoc loc, bool subset)
{
    TIntermTyped* newNode;
    TOperator basicOp;

    //
    // First, convert types as needed.
    //
    switch (op) {
    case EOpConstructVec2:
    case EOpConstructVec3:
    case EOpConstructVec4:
    case EOpConstructMat2x2:
    case EOpConstructMat2x3:
    case EOpConstructMat2x4:
    case EOpConstructMat3x2:
    case EOpConstructMat3x3:
    case EOpConstructMat3x4:
    case EOpConstructMat4x2:
    case EOpConstructMat4x3:
    case EOpConstructMat4x4:
    case EOpConstructFloat:
        basicOp = EOpConstructFloat;
        break;

    case EOpConstructDVec2:
    case EOpConstructDVec3:
    case EOpConstructDVec4:
    case EOpConstructDMat2x2:
    case EOpConstructDMat2x3:
    case EOpConstructDMat2x4:
    case EOpConstructDMat3x2:
    case EOpConstructDMat3x3:
    case EOpConstructDMat3x4:
    case EOpConstructDMat4x2:
    case EOpConstructDMat4x3:
    case EOpConstructDMat4x4:
    case EOpConstructDouble:
        basicOp = EOpConstructDouble;
        break;

    case EOpConstructIVec2:
    case EOpConstructIVec3:
    case EOpConstructIVec4:
    case EOpConstructInt:
        basicOp = EOpConstructInt;
        break;

    case EOpConstructUVec2:
    case EOpConstructUVec3:
    case EOpConstructUVec4:
    case EOpConstructUint:
        basicOp = EOpConstructUint;
        break;

    case EOpConstructBVec2:
    case EOpConstructBVec3:
    case EOpConstructBVec4:
    case EOpConstructBool:
        basicOp = EOpConstructBool;
        break;

    default:
        error(loc, "unsupported construction", "", "");

        return 0;
    }
    newNode = intermediate.addUnaryMath(basicOp, node, node->getLoc());
    if (newNode == 0) {
        error(loc, "can't convert", "constructor", "");
        return 0;
    }

    //
    // Now, if there still isn't an operation to do the construction, and we need one, add one.
    //

    // Otherwise, skip out early.
    if (subset || (newNode != node && newNode->getType() == type))
        return newNode;

    // setAggregateOperator will insert a new node for the constructor, as needed.
    return intermediate.setAggregateOperator(newNode, op, type, loc);
}

// This function tests for the type of the parameters to the structures constructors. Raises
// an error message if the expected type does not match the parameter passed to the constructor.
//
// Returns 0 for an error or the input node itself if the expected and the given parameter types match.
//
TIntermTyped* TParseContext::constructStruct(TIntermNode* node, const TType& type, int paramCount, TSourceLoc loc)
{
    TIntermTyped* converted = intermediate.addConversion(EOpConstructStruct, type, node->getAsTyped());
    if (! converted || converted->getType() != type) {
        error(loc, "", "constructor", "cannot convert parameter %d from '%s' to '%s'", paramCount,
              node->getAsTyped()->getType().getCompleteString().c_str(), type.getCompleteString().c_str());

        return 0;
    }

    return converted;
}

//
// Do everything needed to add an interface block.
//
void TParseContext::addBlock(TSourceLoc loc, TTypeList& typeList, const TString* instanceName, TArraySizes* arraySizes)
{
    // First, error checks

    if (reservedErrorCheck(loc, *blockName))
        return;

    if (instanceName && reservedErrorCheck(loc, *instanceName))
        return;

    if (profile == EEsProfile && arraySizes)
        arraySizeRequiredCheck(loc, arraySizes->getSize());

    switch (currentBlockDefaults.storage) {
    case EvqBuffer:
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "buffer block");
        profileRequires(loc, ECoreProfile | ECompatibilityProfile, 430, 0, "buffer block");
        break;
    case EvqUniform:
        profileRequires(loc, EEsProfile, 300, 0, "uniform block");
        profileRequires(loc, ENoProfile, 140, 0, "uniform block");
        break;
    case EvqVaryingIn:
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "input block");
        break;
    case EvqVaryingOut:
        requireProfile(loc, ECoreProfile | ECompatibilityProfile, "output block");
        break;
    default:
        error(loc, "only uniform, buffer, in, or out blocks are supported", blockName->c_str(), "");
        return;
    }

    arrayDimCheck(loc, arraySizes, 0);

    // fix and check for member qualifiers and types that don't belong within a block
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier& memberQualifier = typeList[member].type->getQualifier();
        TSourceLoc memberLoc = typeList[member].loc;
        pipeInOutFix(memberLoc, memberQualifier);
        if (memberQualifier.storage != EvqTemporary && memberQualifier.storage != EvqGlobal && memberQualifier.storage != currentBlockDefaults.storage)
            error(memberLoc, "member storage qualifier cannot contradict block storage qualifier", typeList[member].type->getFieldName().c_str(), "");
        if ((currentBlockDefaults.storage == EvqUniform && memberQualifier.isInterpolation()) || memberQualifier.isAuxiliary())
            error(memberLoc, "member of uniform block cannot have an auxiliary or interpolation qualifier", typeList[member].type->getFieldName().c_str(), "");

        TBasicType basicType = typeList[member].type->getBasicType();
        if (basicType == EbtSampler)
            error(memberLoc, "member of block cannot be a sampler type", typeList[member].type->getFieldName().c_str(), "");
    }

    // Make default block qualification, and adjust the member qualifications

    TQualifier defaultQualification;
    switch (currentBlockDefaults.storage) {
    case EvqBuffer:     defaultQualification = globalBufferDefaults;     break;
    case EvqUniform:    defaultQualification = globalUniformDefaults;    break;
    case EvqVaryingIn:  defaultQualification = globalInputDefaults;      break;
    case EvqVaryingOut: defaultQualification = globalOutputDefaults;     break;
    default:            defaultQualification.clear();                    break;
    }

    mergeLayoutQualifiers(loc, defaultQualification, currentBlockDefaults);
    for (unsigned int member = 0; member < typeList.size(); ++member) {
        TQualifier memberQualification = defaultQualification;
        mergeQualifiers(loc, memberQualification, typeList[member].type->getQualifier(), false);
        typeList[member].type->getQualifier() = memberQualification;
    }

    // Build and add the interface block as a new type named blockName

    TType blockType(&typeList, *blockName, currentBlockDefaults);
    if (arraySizes)
        blockType.setArraySizes(arraySizes);
    blockType.getQualifier().layoutPacking = defaultQualification.layoutPacking;

    //
    // Don't make a user-defined type out of block name; that will cause an error
    // if the same block name gets reused in a different interface.
    //
    // "Block names have no other use within a shader 
    // beyond interface matching; it is a compile-time error to use a block name at global scope for anything 
    // other than as a block name (e.g., use of a block name for a global variable name or function name is 
    // currently reserved)."
    //
    // Use the symbol table to prevent normal reuse of the block's name, as a variable entry,
    // whose type is EbtBlock, but without all the structure; that will come from the type
    // the instances point to.
    //
    TType blockNameType(EbtBlock);
    TVariable* blockNameVar = new TVariable(blockName, blockNameType);
    if (! symbolTable.insert(*blockNameVar)) {
        TSymbol* existingName = symbolTable.find(*blockName);
        if (existingName->getType().getBasicType() != EbtBlock) {
            error(loc, "block name cannot redefine a non-block name", blockName->c_str(), "");

            return;
        }
    }

    // Add the variable, as anonymous or named instanceName.
    // Make an anonymous variable if no name was provided.
    if (! instanceName)
        instanceName = NewPoolTString("");

    TVariable& variable = *new TVariable(instanceName, blockType);
    if (! symbolTable.insert(variable)) {
        if (*instanceName == "")
            error(loc, "nameless block contains a member that already has a name at global scope", blockName->c_str(), "");
        else
            error(loc, "block instance name redefinition", variable.getName().c_str(), "");

        return;
    }

    // Check for general layout qualifier errors
    layoutCheck(loc, variable);

    // Save it in the AST for linker use.
    intermediate.addSymbolLinkageNode(linkage, variable);
}

// For an identifier that is already declared, add more qualification to it.
void TParseContext::addQualifierToExisting(TSourceLoc loc, TQualifier qualifier, const TString& identifier)
{
    TSymbol* symbol = symbolTable.find(identifier);
    if (! symbol) {
        error(loc, "identifier not previously declared", identifier.c_str(), "");
        return;
    }
    if (symbol->getAsFunction()) {
        error(loc, "cannot re-qualify a function name", identifier.c_str(), "");
        return;
    }

    if (qualifier.isAuxiliary() ||
        qualifier.isMemory() ||
        qualifier.isInterpolation() ||
        qualifier.storage != EvqTemporary ||
        qualifier.precision != EpqNone) {
        error(loc, "cannot add storage, auxiliary, memory, interpolation, or precision qualifier to an existing variable", identifier.c_str(), "");

        return;
    }

    // For read-only built-ins, add a new symbol for holding the modified qualifier.
    // This will bring up an entire block, if a block type has to be modified (e.g., gl_Position inside a block)
    if (symbol->isReadOnly())
        symbol = symbolTable.copyUp(symbol);

    if (qualifier.invariant)
        symbol->getWritableType().getQualifier().invariant = true;
}

void TParseContext::addQualifierToExisting(TSourceLoc loc, TQualifier qualifier, TIdentifierList& identifiers)
{
    for (unsigned int i = 0; i < identifiers.size(); ++i)
        addQualifierToExisting(loc, qualifier, *identifiers[i]);
}

//
// Update qualifier defaults for all forms of declarations, which 
// must error check for their form before calling here.
//
void TParseContext::updateQualifierDefaults(TQualifier qualifier)
{
    switch (qualifier.storage) {
    case EvqBuffer:
        if (qualifier.layoutMatrix != ElmNone)
            globalBufferDefaults.layoutMatrix = qualifier.layoutMatrix;
        if (qualifier.layoutPacking != ElpNone)
            globalBufferDefaults.layoutPacking = qualifier.layoutPacking;
        break;
    case EvqUniform:
        if (qualifier.layoutMatrix != ElmNone)
            globalUniformDefaults.layoutMatrix = qualifier.layoutMatrix;
        if (qualifier.layoutPacking != ElpNone)
            globalUniformDefaults.layoutPacking = qualifier.layoutPacking;
        break;
    case EvqVaryingIn:
        if (qualifier.hasLocation())
            globalInputDefaults.layoutSlotLocation = qualifier.layoutSlotLocation;
        break;
    case EvqVaryingOut:
        if (qualifier.hasLocation())
            globalOutputDefaults.layoutSlotLocation = qualifier.layoutSlotLocation;
        break;
    default:
        // error handling should be done by callers of this function
        break;
    }
}

//
// Update defaults for qualifiers.  This is called directly for the case
// of a declaration with just a qualifier.
//
void TParseContext::updateQualifierDefaults(TSourceLoc loc, TQualifier qualifier)
{
    if (qualifier.isAuxiliary() ||
        qualifier.isMemory() ||
        qualifier.isInterpolation() ||
        qualifier.precision != EpqNone)
        error(loc, "cannot use auxiliary, memory, interpolation, or precision qualifier in a default qualifier declaration (declaration with no type)", "", "");

    switch (qualifier.storage) {
    case EvqUniform:
    case EvqBuffer:
    case EvqVaryingIn:
    case EvqVaryingOut:
        break;
    default:
        error(loc, "default qualifier requires 'uniform', 'buffer', 'in', or 'out' storage qualification", "", "");
        return;
    }
    
    if (qualifier.hasBinding())
        error(loc, "cannot declare a default, include a type or full declaration", "binding", "");
    if (qualifier.hasLocation())
        error(loc, "cannot declare a default, use a full declaration", "location", "");

    updateQualifierDefaults(qualifier);
}

//
// Update defaults for qualifiers when declared with a type, and optionally an identifier.
// (But, not the case of just a qualifier; only when a type is present.)
//
void TParseContext::updateTypedDefaults(TSourceLoc loc, TQualifier qualifier, const TString* id)
{
    bool cantHaveId = false;

    if (! id) {
        if (qualifier.hasLayout())
            warn(loc, "cannot set qualifier defaults when using a type and no identifier", "", "");

        return;
    }

    switch (qualifier.storage) {
    case EvqBuffer:
    case EvqUniform:
        if (qualifier.layoutMatrix != ElmNone)
            error(loc, "cannot specify matrix layout on a variable declaration", id->c_str(), "");
        if (qualifier.layoutPacking != ElpNone)
            error(loc, "cannot specify packing on a variable declaration", id->c_str(), "");
        break;
    case EvqVaryingIn:
        break;
    case EvqVaryingOut:
        break;
    default:
        if (qualifier.layoutMatrix != ElmNone ||
            qualifier.layoutPacking != ElpNone)
            error(loc, "layout qualifiers for matrix layout and packing only apply to uniform or buffer blocks", id->c_str(), "");
        else if (qualifier.hasLocation())
            error(loc, "location qualifiers only appy to uniform, buffer, in, or out storage qualifiers", id->c_str(), "");
    }

    if (cantHaveId)
        error(loc, "cannot set global layout qualifiers on uniform variable, use just 'uniform' or a block", id->c_str(), "");

    updateQualifierDefaults(qualifier);
}

//
// Take the sequence of statements that has been built up since the last case/default,
// put it on the list of top-level nodes for the current (inner-most) switch statement,
// and follow that by the case/default we are on now.  (See switch topology comment on
// TIntermSwitch.)
//
void TParseContext::wrapupSwitchSubsequence(TIntermAggregate* statements, TIntermNode* branchNode)
{
    TIntermSequence* switchSequence = switchSequenceStack.back();

    if (statements) {
        if (switchSequence->size() == 0)
            error(statements->getLoc(), "cannot have statements before first case/default label", "switch", "");
        statements->setOperator(EOpSequence);
        switchSequence->push_back(statements);
    }
    if (branchNode) {
        // check all previous cases for the same label (or both are 'default')
        for (unsigned int s = 0; s < switchSequence->size(); ++s) {
            TIntermBranch* prevBranch = (*switchSequence)[s]->getAsBranchNode();
            if (prevBranch) {
                TIntermTyped* prevExpression = prevBranch->getExpression();
                TIntermTyped* newExpression = branchNode->getAsBranchNode()->getExpression();
                if (prevExpression == 0 && newExpression == 0)
                    error(branchNode->getLoc(), "duplicate label", "default", "");
                else if (prevExpression != 0 &&
                          newExpression != 0 &&
                         prevExpression->getAsConstantUnion() &&
                          newExpression->getAsConstantUnion() &&
                         prevExpression->getAsConstantUnion()->getConstArray()[0].getIConst() ==
                          newExpression->getAsConstantUnion()->getConstArray()[0].getIConst())
                    error(branchNode->getLoc(), "duplicated value", "case", "");
            }
        }
        switchSequence->push_back(branchNode);
    }
}

//
// Turn the top-level node sequence built up of wrapupSwitchSubsequence9)
// into a switch node.
//
TIntermNode* TParseContext::addSwitch(TSourceLoc loc, TIntermTyped* expression, TIntermAggregate* lastStatements)
{
    profileRequires(loc, EEsProfile, 300, 0, "switch statements");
    profileRequires(loc, ENoProfile, 130, 0, "switch statements");

    wrapupSwitchSubsequence(lastStatements, 0);

    if (expression == 0 ||
        (expression->getBasicType() != EbtInt && expression->getBasicType() != EbtUint) ||
        expression->getType().isArray() || expression->getType().isMatrix() || expression->getType().isVector())
            error(loc, "condition must be a scalar integer expression", "switch", "");

    // If there is nothing to do, drop the switch but still execute the expression
    TIntermSequence* switchSequence = switchSequenceStack.back();
    if (switchSequence->size() == 0)
        return expression;

    if (lastStatements == 0) {
        error(loc, "last case/default label must be followed by statements", "switch", "");

        return expression;
    }

    TIntermAggregate* body = new TIntermAggregate(EOpSequence);
    body->getSequence() = *switchSequenceStack.back();
    body->setLoc(loc);

    TIntermSwitch* switchNode = new TIntermSwitch(expression, body);
    switchNode->setLoc(loc);

    return switchNode;
}

//
// This function returns the tree representation for the vector field(s) being accessed from contant vector.
// If only one component of vector is accessed (v.x or v[0] where v is a contant vector), then a contant node is
// returned, else an aggregate node is returned (for v.xy). The input to this function could either be the symbol
// node or it could be the intermediate tree representation of accessing fields in a constant structure or column of
// a constant matrix.
//
TIntermTyped* TParseContext::addConstVectorNode(TVectorFields& fields, TIntermTyped* node, TSourceLoc loc)
{
    TIntermTyped* typedNode;
    TIntermConstantUnion* tempConstantNode = node->getAsConstantUnion();

    TConstUnionArray unionArray;
    if (tempConstantNode)
        unionArray = tempConstantNode->getConstArray();
    else { // The node has to be either a symbol node or an aggregate node or a tempConstant node, else, its an error
        error(loc, "Cannot offset into the vector", "Error", "");

        return 0;
    }

    TConstUnionArray constArray(fields.num);

    for (int i = 0; i < fields.num; i++) {
        if (fields.offsets[i] >= node->getType().getObjectSize()) {
            error(loc, "", "[", "vector index out of range '%d'", fields.offsets[i]);
            fields.offsets[i] = 0;
        }

        constArray[i] = unionArray[fields.offsets[i]];
    }
    typedNode = intermediate.addConstantUnion(constArray, node->getType(), loc);

    return typedNode;
}

//
// This function returns the column being accessed from a constant matrix. The values are retrieved from
// the symbol table and parse-tree is built for a vector (each column of a matrix is a vector). The input
// to the function could either be a symbol node (m[0] where m is a constant matrix)that represents a
// constant matrix or it could be the tree representation of the constant matrix (s.m1[0] where s is a constant structure)
//
TIntermTyped* TParseContext::addConstMatrixNode(int index, TIntermTyped* node, TSourceLoc loc)
{
    TIntermTyped* typedNode;
    TIntermConstantUnion* tempConstantNode = node->getAsConstantUnion();

    if (index >= node->getType().getMatrixCols()) {
        error(loc, "", "[", "matrix field selection out of range '%d'", index);
        index = 0;
    }

    if (tempConstantNode) {
        const TConstUnionArray& unionArray = tempConstantNode->getConstArray();
        int size = tempConstantNode->getType().getMatrixRows();
        // Note: the type is corrected (dereferenced) by the caller
        typedNode = intermediate.addConstantUnion(TConstUnionArray(unionArray, size * index, size), tempConstantNode->getType(), loc);
    } else {
        error(loc, "Cannot offset into the matrix", "Error", "");

        return 0;
    }

    return typedNode;
}


//
// This function returns an element of an array accessed from a constant array. The values are retrieved from
// the symbol table and parse-tree is built for the type of the element. The input
// to the function could either be a symbol node (a[0] where a is a constant array)that represents a
// constant array or it could be the tree representation of the constant array (s.a1[0] where s is a constant structure)
//
TIntermTyped* TParseContext::addConstArrayNode(int index, TIntermTyped* node, TSourceLoc loc)
{
    TIntermTyped* typedNode;
    TIntermConstantUnion* tempConstantNode = node->getAsConstantUnion();
    TType arrayElementType;
    arrayElementType.shallowCopy(node->getType());  // TODO: arrays of arrays: combine this with deref.
    arrayElementType.dereference();

    if (index >= node->getType().getArraySize() || index < 0) {
        error(loc, "", "[", "array index '%d' out of range", index);
        index = 0;
    }

    int arrayElementSize = arrayElementType.getObjectSize();

    if (tempConstantNode) {
         typedNode = intermediate.addConstantUnion(TConstUnionArray(tempConstantNode->getConstArray(), arrayElementSize * index, arrayElementSize),
                                                   tempConstantNode->getType(), loc);
    } else {
        error(loc, "Cannot offset into the array", "Error", "");

        return 0;
    }

    return typedNode;
}


//
// This function returns the value of a particular field inside a constant structure from the symbol table.
// If there is an embedded/nested struct, it appropriately calls addConstStructNested or addConstStructFromAggr
// function and returns the parse-tree with the values of the embedded/nested struct.
//
TIntermTyped* TParseContext::addConstStruct(TString& identifier, TIntermTyped* node, TSourceLoc loc)
{
    TTypeList* fields = node->getType().getStruct();
    TIntermTyped *typedNode;
    int instanceOffset = 0;
    int instanceSize;
    unsigned int index = 0;
    TIntermConstantUnion *tempConstantNode = node->getAsConstantUnion();

    for ( index = 0; index < fields->size(); ++index) {
        instanceSize = (*fields)[index].type->getObjectSize();

        if ((*fields)[index].type->getFieldName() == identifier)
            break;

        instanceOffset += instanceSize;
    }

    if (tempConstantNode) {
         typedNode = intermediate.addConstantUnion(TConstUnionArray(tempConstantNode->getConstArray(), instanceOffset, instanceSize),
                                                   tempConstantNode->getType(), loc);
         // type will be changed in the calling function
    } else {
        error(loc, "Cannot offset into the structure", "Error", "");

        return 0;
    }

    return typedNode;
}

} // end namespace glslang
