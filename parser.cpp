#include "lexer.h"
#include "parser.h"
#include "expr.h"
#include "namedobject.h"
#include "stack.h"
#include "builtin.h"
#include "options.h"
#include "trace.h"
#include <iostream>
#include <cassert>
#include <limits>
#include <vector>
#include <algorithm>
#include <cmath>

class UpdateCallVisitor : public Visitor
{
public:
    UpdateCallVisitor(const PrototypeAST *p) : proto(p) {}
    virtual void visit(ExprAST* expr);
private:
    const PrototypeAST* proto;
};

void UpdateCallVisitor::visit(ExprAST* expr)
{
    TRACE();

    if (verbosity > 1)
    {
	expr->dump();
    }

    if(CallExprAST* call = llvm::dyn_cast<CallExprAST>(expr))
    {
	if (call->Proto()->Name() == proto->Name()
	    && call->Args().size() != proto->Args().size())
	{
	    if (verbosity)
	    {
		std::cerr << "Adding arguments for function" << std::endl;
	    }
	    auto& args = call->Args();
	    for(auto u : proto->Function()->UsedVars())
	    {
		args.push_back(new VariableExprAST(call->Loc(), u.Name(), u.Type()));
	    }
	}
    }
}

Parser::Parser(Lexer &l)
    : lexer(l), nextTokenValid(false), errCnt(0)
{
    if (!(AddType("integer", new Types::IntegerDecl) &&
	  AddType("longint", new Types::Int64Decl) &&
	  AddType("real", new Types::RealDecl) &&
	  AddType("char", new Types::CharDecl) &&
	  AddType("boolean", new Types::BoolDecl) &&
	  AddType("text", Types::GetTextType())))
    {
	assert(0 && "Failed to add basic types...");
    }

    if (!(AddConst("pi", new Constants::RealConstDecl(Location("", 0, 0), M_PI))))
    {
	assert(0 && "Failed to add builtin constants");
    }
}

ExprAST* Parser::Error(const std::string& msg, const char* file, int line)
{
    if (file)
    {
	std::cerr << file << ":" << line << ": ";
    }
    std::cerr << "Error: " << msg << std::endl;
    errCnt++;
    return 0;
}

PrototypeAST* Parser::ErrorP(const std::string& msg)
{
    Error(msg);
    return 0;
}

FunctionAST* Parser::ErrorF(const std::string& msg)
{
    Error(msg);
    return 0;
}

Types::TypeDecl* Parser::ErrorT(const std::string& msg)
{
    Error(msg);
    return 0;
}

Types::RangeDecl* Parser::ErrorR(const std::string& msg)
{
    Error(msg);
    return 0;
}

VariableExprAST* Parser::ErrorV(const std::string& msg)
{
    Error(msg);
    return 0;
}

const Token& Parser::CurrentToken() const
{
    return curToken;
}

const Token& Parser::NextToken(const char* file, int line)
{
    if (nextTokenValid)
    {
	curToken = nextToken;
	nextTokenValid = false;
    }
    else
    {
	curToken = lexer.GetToken();
    }
    if (verbosity)
    {
	curToken.dump(std::cerr, file, line);
    }
    return curToken;
}

const Token& Parser::PeekToken(const char* file, int line) 
{
    if (nextTokenValid)
    {
	return nextToken;
    }
    else
    {
	nextTokenValid = true;
	nextToken = lexer.GetToken();
    }
    if (verbosity > 1)
    {
	std::cerr << "peeking: ";
	nextToken.dump(std::cerr, file, line);
    }
    return nextToken;
}

bool Parser::Expect(Token::TokenType type, bool eatIt, const char* file, int line)
{
    if (CurrentToken().GetToken() != type)
    {
	Token t(type, Location("", 0, 0));
	Error(std::string("Expected '") + t.TypeStr() + "', got '" +  CurrentToken().ToString() + 
	      "'.", file, line);
	return false;
    }
    if (eatIt)
    {
	NextToken(file, line);
    }
    return true;
}

bool Parser::ExpectSemicolonOrEnd(const char* file, int line)
{
    return !(CurrentToken().GetToken() != Token::End && !Expect(Token::Semicolon, true, file, line));
}

#define NextToken() NextToken(__FILE__, __LINE__)
#define PeekToken() PeekToken(__FILE__, __LINE__)
#define ExpectSemicolonOrEnd() ExpectSemicolonOrEnd(__FILE__, __LINE__)
#define Expect(t, e) Expect(t, e, __FILE__, __LINE__)

Types::TypeDecl* Parser::GetTypeDecl(const std::string& name)
{
    if (NamedObject* def = nameStack.Find(name))
    {
	if (TypeDef *typeDef = llvm::dyn_cast<TypeDef>(def))
	{
	    return typeDef->Type();
	}
    }
    return 0;
}

ExprAST* Parser::ParseNilExpr()
{
    if (Expect(Token::Nil, true))
    {
	return new NilExprAST(CurrentToken().Loc());
    }
    return 0;
}

ExprAST* Parser::ParseSizeOfExpr()
{
    if (!Expect(Token::SizeOf, true))
    {
	return 0;
    }
    if (!Expect(Token::LeftParen, true))
    {
	return 0;
    }
    ExprAST* expr = 0;
    if (CurrentToken().GetToken() == Token::Identifier)
    {
	if (Types::TypeDecl* ty = GetTypeDecl(CurrentToken().GetIdentName()))
	{
	    expr = new SizeOfExprAST(CurrentToken().Loc(), ty);
	    NextToken();
	}
    }
    if (!expr)
    {
	if (ExprAST* e = ParseExpression())
	{
	    expr = new SizeOfExprAST(CurrentToken().Loc(), e->Type());
	}
    }
    if (!Expect(Token::RightParen, true))
    {
	return 0;
    }
    return expr;
}

const Constants::ConstDecl* Parser::GetConstDecl(const std::string& name)
{
    if (ConstDef *constDef = llvm::dyn_cast_or_null<ConstDef>(nameStack.Find(name)))
    {
	return constDef->ConstValue();
    }
    return 0;
}

EnumDef* Parser::GetEnumValue(const std::string& name)
{
    return llvm::dyn_cast_or_null<EnumDef>(nameStack.Find(name));
}

bool Parser::AddType(const std::string& name, Types::TypeDecl* ty)
{
    if (Types::EnumDecl* ed = llvm::dyn_cast<Types::EnumDecl>(ty))
    {
	for(auto v : ed->Values())
	{
	    if (!nameStack.Add(v.name, new EnumDef(v.name, v.value, ty)))
	    {
		Error("Enumerated value by name " + v.name + " already exists...");
		return false;
	    }
	}
    }
    return nameStack.Add(name, new TypeDef(name, ty));
}

bool Parser::AddConst(const std::string& name, const Constants::ConstDecl* cd)
{
    if (!nameStack.Add(name, new ConstDef(name, cd)))
    {
	Error(std::string("Name ") + name + " is already declared as a constant");
	return false;
    }
    return true;

}

Types::TypeDecl* Parser::ParseSimpleType()
{
    if (CurrentToken().GetToken() != Token::Identifier)
    {
	return ErrorT("Expected identifier of simple type");
    }
    if (Types::TypeDecl* ty = GetTypeDecl(CurrentToken().GetIdentName()))
    {
	NextToken();
	return ty;
    }
    return ErrorT("Identifier does not name a type");
}

void Parser::TranslateToken(Token& token)
{
    if (token.GetToken() == Token::Identifier)
    {
	if (const Constants::ConstDecl* cd = GetConstDecl(token.GetIdentName()))
	{
	    token = cd->Translate();
	}
    }
}

int Parser::ParseConstantValue(Token::TokenType& tt, Types::TypeDecl*& type)
{
    Token token = CurrentToken();
    TranslateToken(token);

    if (tt != Token::Unknown && token.GetToken() != tt)
    {
	Error("Expected token to match type"); 
	tt = Token::Unknown;
	return 0;
    }

    tt = token.GetToken();

    int result = 0;

    switch(tt)
    {
    case Token::Integer:
	type = new Types::IntegerDecl;
	result = token.GetIntVal();
	break;

    case Token::Char:
	type = new Types::CharDecl;
	result = token.GetIntVal();
	break;

    case Token::Identifier:
    {
	tt = CurrentToken().GetToken();
	
	if (EnumDef* ed = GetEnumValue(CurrentToken().GetIdentName()))
	{
	    type = ed->Type();
	    result = ed->Value();
	}
	else
	{
	    tt = Token::Unknown;
	    Error("Invalid constant, expected identifier for enumerated type");
	    return 0;
	}
	break;
    }
    default:
	tt = Token::Unknown;
	Error("Invalid constant value, expected char, integer or enum value");
	return 0;
    }
    NextToken();
    return result;
}

Types::RangeDecl* Parser::ParseRange(Types::TypeDecl*& type)
{
    Token::TokenType tt = Token::Unknown;

    int start = ParseConstantValue(tt, type);
    if (tt == Token::Unknown)
    {
	return 0;
    }
    if (!Expect(Token::DotDot, true))
    {
	return 0;
    }

    int end = ParseConstantValue(tt, type);

    if (tt == Token::Unknown)
    {
	return 0;
    }

    if (end <= start)
    {
	return ErrorR("Invalid range specification");
    }
    return new Types::RangeDecl(new Types::Range(start, end), type->Type());
}

Types::RangeDecl* Parser::ParseRangeOrTypeRange(Types::TypeDecl*& type)
{
    if (CurrentToken().GetToken() == Token::Identifier)
    {
	if ((type = GetTypeDecl(CurrentToken().GetIdentName())))
	{
	    if (!type->isIntegral())
	    {
		return ErrorR("Type used as index specification should be integral type");
	    }
	    NextToken();
	    return new Types::RangeDecl(type->GetRange(), type->Type());
	}
    }

    return ParseRange(type);
}

Constants::ConstDecl* Parser::ParseConstEval(const Constants::ConstDecl* lhs, 
					     const Token& binOp, 
					     const Constants::ConstDecl* rhs)
{
    switch(binOp.GetToken())
    {
    case Token::Plus:
	return *lhs + *rhs;
	break;

    case Token::Minus:
	return *lhs - *rhs;
	break;

    case Token::Multiply:
	return *lhs * *rhs;

    default:
	break;
    }
    return 0;
}

const Constants::ConstDecl* Parser::ParseConstRHS(int exprPrec, const Constants::ConstDecl* lhs)
{
    for(;;)
    {
	int tokPrec = CurrentToken().Precedence();
	if (tokPrec < exprPrec)
	{
	    return lhs;
	}
	
	Token binOp = CurrentToken();
	NextToken();

	const Constants::ConstDecl* rhs = ParseConstExpr();
	if (!rhs)
	{
	    return 0;
	}
	
	int nextPrec = CurrentToken().Precedence();
	if (verbosity)
	{
	    CurrentToken().dump(std::cerr);
	    std::cerr << " tokprec=" << tokPrec << " nextPrec="  << nextPrec << std::endl;
	}
	if (tokPrec < nextPrec)
	{
	    std::cout << "Going deeper!";
	    std::cout << " lhs="; lhs->dump();
	    std::cout << " "; binOp.dump();
	    std::cout << " rhs="; rhs->dump();
	    rhs = ParseConstRHS(tokPrec + 1, rhs);
	    if (!rhs)
	    {
		return 0;
	    }
	}
	lhs->dump();
	binOp.dump();
	rhs->dump();
	std::cout << std::endl;
	lhs = ParseConstEval(lhs, binOp, rhs);
    }
}

const Constants::ConstDecl* Parser::ParseConstExpr()
{
    Token::TokenType unaryToken = Token::Unknown;
    Location loc = CurrentToken().Loc();
    const Constants::ConstDecl* cd;
    int mul = 1;
    do
    {
	if (verbosity)
	{
	    std::cerr << __FILE__ << ":" << __LINE__ << ": ";
	    CurrentToken().dump(std::cerr);
	}

	switch(CurrentToken().GetToken())
	{
	case Token::Minus:
	    mul = -1;
	case Token::Plus:
	case Token::Not:
	    unaryToken = CurrentToken().GetToken();
	    break;

	case Token::LeftParen:
	    NextToken();
	    cd = ParseConstExpr();
	    // We don't eat the right paren here, it gets eaten later.
	    if (!Expect(Token::RightParen, false))
	    {
		return 0;
	    }
	    break;

	case Token::StringLiteral:
	    if (unaryToken != Token::Unknown)
	    {
		Error("Unary + or - not allowed for string constants");
		return 0;
	    }
	    cd = new Constants::StringConstDecl(loc, CurrentToken().GetStrVal());
	    break;
	    
	case Token::Integer:
	{
	    long v = CurrentToken().GetIntVal();
	    if (unaryToken == Token::Not)
	    {
		v = ~v;
	    }
	    cd = new Constants::IntConstDecl(loc,  v * mul);
	    break;
	}
	    
	case Token::Real:
	    if (unaryToken == Token::Not)
	    {
		Error("Unary 'not' is not allowed for real constants");
		return 0;
	    }
	    cd = new Constants::RealConstDecl(loc, CurrentToken().GetRealVal() * mul);
	    break;
	    
	case Token::Char:
	    if (unaryToken != Token::Unknown)
	    {
		Error("Unary + or - not allowed for char constants");
		return 0;
	    }
	    cd = new Constants::CharConstDecl(loc, (char) CurrentToken().GetIntVal());
	    break;

	case Token::Identifier:
	{
	    EnumDef* ed = GetEnumValue(CurrentToken().GetIdentName());
	    if (ed)
	    {
		if (ed->Type()->Type() == Types::Boolean)
		{
		    long v = ed->Value();
		    if (unaryToken == Token::Not)
		    {
			v = !v;
		    }
		    else if (unaryToken != Token::Unknown)
		    {
			Error("Unary + or - not allowed for bool constants");
			return 0;
		    }
		    cd = new Constants::BoolConstDecl(loc, v);
		}
		else
		{
		    if (unaryToken != Token::Unknown)
		    {
			Error("Unary + or - not allowed for enum constants");
			return 0;
		    }
		    cd = new Constants::IntConstDecl(loc, ed->Value());
		}
	    }
	    else 
	    {
		cd = GetConstDecl(CurrentToken().GetIdentName());
		assert(cd && "Expected to get an identifier!");
		if (llvm::isa<Constants::BoolConstDecl>(cd) && unaryToken == Token::Not)
		{
		    const Constants::BoolConstDecl* bd =
			llvm::dyn_cast<Constants::BoolConstDecl>(cd);
		    cd = new Constants::BoolConstDecl(loc, !bd->Value());
		}
		
		if (mul == -1)
		{
		    if(llvm::isa<Constants::RealConstDecl>(cd))
		    {
			const Constants::RealConstDecl* rd =
			    llvm::dyn_cast<Constants::RealConstDecl>(cd);
			cd = new Constants::RealConstDecl(loc, -rd->Value());
		    }
		    else if (llvm::isa<Constants::IntConstDecl>(cd))
		    {
			const Constants::IntConstDecl* id =
			    llvm::dyn_cast<Constants::IntConstDecl>(cd);
			cd = new Constants::IntConstDecl(loc, -id->Value());
		    }
		    else
		    {
			Error("Can't negate the type of " + CurrentToken().GetIdentName() + 
			      " only integer and real types can be negated");
			return 0;
		    }
		}
	    }
	    break;
	}
	
	default:
	    return 0;
	}
	NextToken();
	if (CurrentToken().GetToken() != Token::Semicolon &&
	    CurrentToken().GetToken() != Token::RightParen)
	{
	    cd = ParseConstRHS(0, cd);
	}
    } while(CurrentToken().GetToken() != Token::Semicolon &&
	    CurrentToken().GetToken() != Token::RightParen);
    return cd;
}

void Parser::ParseConstDef()
{
    if (!Expect(Token::Const, true))
    {
	return;
    }
    do
    {
	if (!Expect(Token::Identifier, false))
	{
	    return;
	}
	std::string nm = CurrentToken().GetIdentName();
	NextToken();
	if (!Expect(Token::Equal, true))
	{
	    return;
	}
	const Constants::ConstDecl *cd = ParseConstExpr();
	if (!cd) 
	{
	    Error("Invalid constant value");
	}
	if (!AddConst(nm, cd))
	{
	    return;
	}
	if (!Expect(Token::Semicolon, true))
	{
	    return;
	}
    } while (CurrentToken().GetToken() == Token::Identifier);
}

// Deal with type name = ... defintions
void Parser::ParseTypeDef()
{
    std::vector<Types::PointerDecl*> incomplete;
    if (!Expect(Token::Type, true))
    {
	return;
    }
    do
    {
	if (!Expect(Token::Identifier, false))
	{
	    return;
	}
	std::string nm = CurrentToken().GetIdentName();
	NextToken();
	if (!Expect(Token::Equal, true))
	{
	    return;
	}
	if (Types::TypeDecl* ty = ParseType(nm))
	{
	    if (!AddType(nm,  ty))
	    {
		Error(std::string("Name ") + nm + " is already in use.");
	    }
	    if (ty->Type() == Types::PointerIncomplete)
	    {
		incomplete.push_back(llvm::dyn_cast<Types::PointerDecl>(ty));
	    }	    
	    if (!Expect(Token::Semicolon, true))
	    {
		return;
	    }
	}
	else
	{
	    return;
	}
    } while (CurrentToken().GetToken() == Token::Identifier);

    // Now fix up any incomplete types...
    for(auto p : incomplete)
    {
	if (Types::TypeDecl *ty = GetTypeDecl(p->Name()))
	{
	    p->SetSubType(ty);
	}
	else
	{
	    Error("Forward declared pointer type not declared: " + p->Name());
	    return;
	}
    }
}

Types::EnumDecl* Parser::ParseEnumDef()
{
    if (!Expect(Token::LeftParen, true))
    {
	return 0;
    }
    std::vector<std::string> values;
    while(CurrentToken().GetToken() != Token::RightParen)
    {
	if (!Expect(Token::Identifier, false))
	{
	    return 0;
	}
	values.push_back(CurrentToken().GetIdentName());
	NextToken();
	if (CurrentToken().GetToken() != Token::RightParen)
	{
	    if (!Expect(Token::Comma, true))
	    {
		return 0;
	    }
	}
    }
    if (!Expect(Token::RightParen, true))
    {
	return 0;
    }
    return new Types::EnumDecl(values);
}

Types::PointerDecl* Parser::ParsePointerType()
{
    if (!Expect(Token::Uparrow, true))
    {
	return 0;
    }
    // If the name is an "identifier" then it's a name of a not yet declared type.
    // We need to forward declare it, and backpatch later.
    if (CurrentToken().GetToken() == Token::Identifier)
    {
	std::string name = CurrentToken().GetIdentName();
	// Is it a known type?
	NextToken();
	if (Types::TypeDecl* ty = GetTypeDecl(name))
	{
	    return new Types::PointerDecl(ty);	
	}
	// Otherwise, forward declare... 
	return new Types::PointerDecl(name);
    }
    else
    {
	return new Types::PointerDecl(ParseType(""));
    }
}

Types::ArrayDecl* Parser::ParseArrayDecl()
{
    if (!Expect(Token::Array, true))
    {
	return 0;
    }
    if (!Expect(Token::LeftSquare, true))
    {
	return 0;
    }
    std::vector<Types::RangeDecl*> rv;
    Types::TypeDecl* type = NULL;
    while(CurrentToken().GetToken() != Token::RightSquare)
    {
	if (Types::RangeDecl* r = ParseRangeOrTypeRange(type))
	{
	    assert(type && "Uh? Type is supposed to be set now");
	    rv.push_back(r);
	}
	else
	{
	    return 0;
	}
	if (CurrentToken().GetToken() == Token::Comma)
	{
	    NextToken();
	}
    }
    if (!Expect(Token::RightSquare, true) || 
	!Expect(Token::Of, true))
    {
	return 0;
    }
    Types::TypeDecl* ty = ParseType("");
    if (!ty)
    {
	return 0;
    }
    return new Types::ArrayDecl(ty, rv);
}

Types::VariantDecl* Parser::ParseVariantDecl(Types::TypeDecl*& type)
{
    Token::TokenType tt = Token::Unknown;
    std::vector<int> variantsSeen;
    std::vector<Types::FieldDecl*> variants;
    do
    {
	do
	{
	    int v = ParseConstantValue(tt, type);
	    if (tt == Token::Unknown)
	    {
		return 0;
	    }
	    if (std::find(variantsSeen.begin(), variantsSeen.end(), v) != variantsSeen.end())
	    {
		Error(std::string("Value already used: ") + std::to_string(v) + " in variant declaration");
		return 0;
	    }
	    variantsSeen.push_back(v);
	    if (CurrentToken().GetToken() != Token::Colon)
	    {
		if (!Expect(Token::Comma, true))
		{
		    return 0;
		}
	    }
	} while (CurrentToken().GetToken() != Token::Colon);
	if (!Expect(Token::Colon, true))
	{
	    return 0;
	}
	if (!Expect(Token::LeftParen, true))
	{
	    return 0;
	}
	std::vector<Types::FieldDecl*> fields; 
	do 
	{
	    std::vector<std::string> names;
	    do 
	    {
		// TODO: Fix up to reduce duplication of this code. It's in several places now.
		if (!Expect(Token::Identifier, false))
		{
		    return 0;
		}
		names.push_back(CurrentToken().GetIdentName());
		NextToken();
		if (CurrentToken().GetToken() != Token::Colon)
		{
		    if (!Expect(Token::Comma, true))
		    {
			return 0;
		    }
		}
	    } while(CurrentToken().GetToken() != Token::Colon);
	    if (!Expect(Token::Colon, true))
	    {
		return 0;
	    }
	    if (Types::TypeDecl* ty = ParseType(""))
	    {
		for(auto n : names)
		{
		    for(auto f : fields)
		    {
			if (n == f->Name())
			{
			    Error(std::string("Duplicate field name '") + n + "' in record");
			    return 0;
			}
		    }
		    fields.push_back(new Types::FieldDecl(n, ty));
		}
		if (CurrentToken().GetToken() != Token::RightParen)
		{
		    if (!Expect(Token::Semicolon, true))
		    {
			return 0;
		    }
		}
	    }
	    else
	    {
		return 0;
	    }
	} while(CurrentToken().GetToken() != Token::RightParen);
	if (!Expect(Token::RightParen, true))
	{
	}
	if (!ExpectSemicolonOrEnd())
	{
	    return 0;
	}
	if (fields.size() == 1)
	{
	    variants.push_back(fields[0]);
	}
	else
	{
	    variants.push_back(new Types::FieldDecl("", new Types::RecordDecl(fields, 0)));
	}
    } while (CurrentToken().GetToken() != Token::End);
    return new Types::VariantDecl(variants);
}


bool Parser::ParseFields(std::vector<Types::FieldDecl*>& fields, Types::VariantDecl*& variant,
			 Token::TokenType type)
{
    bool isObject = type == Token::Object;
    variant = 0;
    do
    {
	std::vector<std::string> names;
	// Parse Variant part if we have a "case". 
	if (CurrentToken().GetToken() == Token::Case)
	{
	    NextToken();
	    std::string marker = "";
	    Types::TypeDecl* markerTy;
	    if (CurrentToken().GetToken() == Token::Identifier && 
		PeekToken().GetToken() == Token::Colon)
	    {
		marker = CurrentToken().GetIdentName();
		NextToken();
		if (!Expect(Token::Colon, true))
		{
		    return false;
		}
	    }
	    markerTy = ParseType("");
	    if (!markerTy->isIntegral())
	    {
		Error("Expect variant selector to be integral type");
		return false;
	    }
	    if (marker != "")
	    {
		fields.push_back(new Types::FieldDecl(marker, markerTy));
	    }
	    if (!Expect(Token::Of, true))
	    {
		return false;
	    }
	    Types::TypeDecl* type;
	    variant =  ParseVariantDecl(type);
	    if(!variant)
	    {
		return false;
	    }
	    if (*markerTy != *type)
	    {
		Error("Marker type does not match member variant type");
		return false;
	    }
	}
	else if (isObject && 
		 (CurrentToken().GetToken() == Token::Function ||
		  CurrentToken().GetToken() == Token::Procedure))
	{
	    PrototypeAST* p = ParsePrototype();
	    int f = 0;
	    if (CurrentToken().GetToken() == Token::Static)
	    {
		f |= Types::MemberFuncDecl::Static;
		NextToken();
		if (!Expect(Token::Semicolon, true))
		{
		    return false;
		}
	    }
	    if (CurrentToken().GetToken() == Token::Virtual)
	    {
		f |= Types::MemberFuncDecl::Virtual;
		NextToken();
		if (!Expect(Token::Semicolon, true))
		{
		    return false;
		}
	    }
	    Types::MemberFuncDecl* m = new Types::MemberFuncDecl(p, f);
	    fields.push_back(new Types::FieldDecl(p->Name(), m));
	}
	else
	{
	    do
	    {
		if (!Expect(Token::Identifier, false))
		{
		    return false;
		}
		names.push_back(CurrentToken().GetIdentName());
		NextToken();
		if (CurrentToken().GetToken() != Token::Colon)
		{
		    if (!Expect(Token::Comma, true))
		    {
			return false;
		    }
		}
	    } while(CurrentToken().GetToken() != Token::Colon);
	    if (!Expect(Token::Colon, true))
	    {
		return false;
	    }
	    if (names.size() == 0)
	    {
		assert(0 && "Should have at least one name declared?");
		return false;
	    }
	    if (Types::TypeDecl* ty = ParseType(""))
	    {
		for(auto n : names)
		{
		    for(auto f : fields)
		    {
			if (n == f->Name())
			{
			    Error(std::string("Duplicate field name '") + n + "' in record");
			    return false;
			}
		    }
		    fields.push_back(new Types::FieldDecl(n, ty));
		}
	    }
	    else
	    {
		return false;
	    }
	    if (!ExpectSemicolonOrEnd())
	    {
		return false;
	    }
	}
    } while(CurrentToken().GetToken() != Token::End);
    if (!Expect(Token::End, true))
    {
	return false;
    }
    return true;
}

Types::RecordDecl* Parser::ParseRecordDecl()
{
    if (!Expect(Token::Record, true))
    {
	return 0;
    }
    std::vector<Types::FieldDecl*> fields;
    Types::VariantDecl* variant;
    if (!ParseFields(fields, variant, Token::Record))
    {
	return 0;
    }
    if (fields.size() == 0 && !variant)
    {
	Error("No elements in record declaration");
	return 0;
    }
    return new Types::RecordDecl(fields, variant);
}

Types::FileDecl* Parser::ParseFileDecl()
{
    if (!Expect(Token::File, true))
    {
	return 0;
    }

    if (!Expect(Token::Of, true))
    {
	return 0;
    }

    Types::TypeDecl* type = ParseType("");

    return new Types::FileDecl(type);
}

Types::SetDecl* Parser::ParseSetDecl()
{
    if (!Expect(Token::Set, true))
    {
	return 0;
    }
    if (!Expect(Token::Of, true))
    {
	return 0;
    }

    Types::TypeDecl* type;
    if (Types::RangeDecl* r = ParseRangeOrTypeRange(type))
    {
	assert(type && "Uh? Type is supposed to be set");
	return new Types::SetDecl(r, type);
    }
    return 0;
}

Types::StringDecl* Parser::ParseStringDecl()
{
    if (!Expect(Token::String, true))
    {
	return 0;
    }

    unsigned size = 255;

    if (CurrentToken().GetToken() == Token::LeftSquare)
    {
	NextToken();
	Token token = CurrentToken();
	TranslateToken(token);

	if (token.GetToken() != Token::Integer)
	{
	    Error("Expected integer value!");
	    return 0;
	}

	size = token.GetIntVal();
	
	NextToken();
	if (!Expect(Token::RightSquare, true))
	{
	    return 0;
	}
    }
    return new Types::StringDecl(size);
}

Types::ObjectDecl* Parser::ParseObjectDecl(const std::string &name)
{
    if (!Expect(Token::Object, true))
    {
	return 0;
    }
    Types::ObjectDecl* base = 0;
    // Find derived class, if available.
    if (CurrentToken().GetToken() == Token::LeftParen)
    {
	NextToken();
	if (!Expect(Token::Identifier, false))
	{
	    return 0;
	}
	std::string baseName = CurrentToken().GetIdentName();
	if (!(base = llvm::dyn_cast_or_null<Types::ObjectDecl>(GetTypeDecl(baseName))))
	{
	    Error("Expected object as base");
	    return 0;
	}   
	NextToken();
	if (!Expect(Token::RightParen, true))
	{
	    return 0;
	}
    }
    
    std::vector<Types::FieldDecl*> fields;
    Types::VariantDecl* variant;
    if (!ParseFields(fields, variant, Token::Object))
    {
	return 0;
    }

    std::vector<Types::MemberFuncDecl*> mf;
    for(auto f = fields.begin(); f != fields.end(); )
    {
	if (Types::MemberFuncDecl* m = llvm::dyn_cast<Types::MemberFuncDecl>((*f)->FieldType()))
	{
	    mf.push_back(m);
	    f = fields.erase(f);
	}
	else
	{
	    f++;
	}
    }

    return new Types::ObjectDecl(name, fields, mf, variant, base);
}

Types::TypeDecl* Parser::ParseType(const std::string& name)
{
    Token::TokenType tt = CurrentToken().GetToken();
    if (tt == Token::Packed)
    {
	tt = NextToken().GetToken();
	if (tt != Token::Array && tt != Token::Record)
	{
	    return ErrorT("Expected 'array' or 'record' after 'packed'");
	}
    }

    switch(tt)
    {
    case Token::Identifier:
    {
	if (!GetEnumValue(CurrentToken().GetIdentName()))
	{
	    return ParseSimpleType();
	}
    }
    // Fall through:
    case Token::Integer:
    case Token::Char:
    {
	Types::TypeDecl*  type = NULL;
	if (Types::RangeDecl* r = ParseRange(type))
	{
	    return r;
	}
	return 0;
    }

    case Token::Array:
	return ParseArrayDecl();

    case Token::Record:
	return ParseRecordDecl();

    case Token::Object:
	return ParseObjectDecl(name);

    case Token::File:
	return ParseFileDecl();

    case Token::Set:
	return ParseSetDecl();

    case Token::LeftParen:
	return ParseEnumDef();

    case Token::Uparrow:
	return ParsePointerType();

    case Token::String:
	return ParseStringDecl();
	break;

    default:
	return ErrorT("Can't understand type");
    }
}

ExprAST* Parser::ParseIntegerExpr(Token token)
{
    long val = token.GetIntVal();
    Location loc = token.Loc();
    ExprAST* result;
    if (val < std::numeric_limits<unsigned int>::min() || val > std::numeric_limits<unsigned int>::max())
    {
	result = new IntegerExprAST(loc, val, GetTypeDecl("longint"));
    }
    else
    {
	result = new IntegerExprAST(loc, val, GetTypeDecl("integer"));
    }
    NextToken();
    return result;
}

ExprAST* Parser::ParseCharExpr(Token token)
{
    ExprAST* result = new CharExprAST(token.Loc(), token.GetIntVal(), GetTypeDecl("char"));
    NextToken();
    return result;
}

ExprAST* Parser::ParseRealExpr(Token token)
{
    ExprAST* result = new RealExprAST(token.Loc(), token.GetRealVal(), GetTypeDecl("real"));
    NextToken();
    return result;
}

ExprAST* Parser::ParseStringExpr(Token token)
{
    int len =  token.GetStrVal().length()-1;
    if (len < 1)
    {
	len = 1;
    }
    std::vector<Types::RangeDecl*> rv{new Types::RangeDecl(new Types::Range(0, len), Types::Integer)};
    Types::ArrayDecl *ty = new Types::ArrayDecl(GetTypeDecl("char"), rv);
    ExprAST* result = new StringExprAST(token.Loc(), token.GetStrVal(), ty);
    NextToken();
    return result;
}

ExprAST* Parser::ParseBinOpRHS(int exprPrec, ExprAST* lhs)
{
    for(;;)
    {
	int tokPrec = CurrentToken().Precedence();
	if (tokPrec < exprPrec)
	{
	    return lhs;
	}

	Token binOp = CurrentToken();
	NextToken();
	
	ExprAST* rhs = ParsePrimary();
	if (!rhs)
	{
	    return 0;
	}

	// If the new operator binds less tightly, take it as LHS of 
	// the next operator. 
	int nextPrec = CurrentToken().Precedence();
	if (tokPrec < nextPrec)
	{
	    rhs = ParseBinOpRHS(tokPrec+1, rhs);
	    if (!rhs) 
	    {
		return 0;
	    }
	}

	// Now combine to new lhs. 
	lhs = new BinaryExprAST(binOp, lhs, rhs);
    }
}

ExprAST* Parser::ParseUnaryOp()
{
    assert((CurrentToken().GetToken() == Token::Minus || 
	    CurrentToken().GetToken() == Token::Plus || 
	    CurrentToken().GetToken() == Token::Not) && 
	   "Expected only minus at this time as a unary operator");

    Token oper = CurrentToken();

    NextToken();

    if (ExprAST* rhs = ParsePrimary())
    {
	// unary + = no change, so just return the expression.
	if (oper.GetToken() == Token::Plus)
	{
	    return rhs;
	}
	return new UnaryExprAST(oper.Loc(), oper, rhs);
    }
    return 0;
}

ExprAST* Parser::ParseExpression()
{
    ExprAST* lhs = ParsePrimary();
    if (!lhs)
    {
	return 0;
    }
    return ParseBinOpRHS(0, lhs);
}

VariableExprAST* Parser::ParseArrayExpr(VariableExprAST* expr, Types::TypeDecl*& type)
{
    Types::ArrayDecl* adecl = llvm::dyn_cast<Types::ArrayDecl>(type);
    if (!adecl)
    {
	return ErrorV("Expected variable of array type when using index");
    }
    NextToken();
    std::vector<ExprAST*> indices;
    while(CurrentToken().GetToken() != Token::RightSquare)
    {
	ExprAST* index = ParseExpression();
	if (!index)
	{
	    return ErrorV("Expected index expression");
	}
	indices.push_back(index);
	if (indices.size() == adecl->Ranges().size())
	{
	    expr = new ArrayExprAST(CurrentToken().Loc(), expr, indices, adecl->Ranges(), adecl->SubType());
	    indices.clear();
	    type = adecl->SubType();
	    adecl = llvm::dyn_cast<Types::ArrayDecl>(type);
	}
	if (CurrentToken().GetToken() != Token::RightSquare)
	{
	    if (!Expect(Token::Comma, true) || !adecl)
	    {
		return 0;
	    }
	}
    }
    if (!Expect(Token::RightSquare, true))
    {
	return 0;
    }
    if (indices.size())
    {
	expr = new ArrayExprAST(CurrentToken().Loc(), expr, indices, adecl->Ranges(), adecl->SubType());
	type = adecl->SubType();
    }
    return expr;
}

ExprAST* Parser::MakeCallExpr(VariableExprAST* self,
			      NamedObject* def,
			      const std::string& funcName,
			      std::vector<ExprAST*>& args)
{
    TRACE();

    const PrototypeAST* proto = 0;
    ExprAST* expr = 0;
    if (const FuncDef *funcDef = llvm::dyn_cast_or_null<const FuncDef>(def))
    {
	proto = funcDef->Proto();
	expr = new FunctionExprAST(CurrentToken().Loc(), funcName, funcDef->Type());
    }
    else if (llvm::dyn_cast_or_null<const VarDef>(def))
    {
	if (def->Type()->Type() == Types::Pointer)
	{
	    Types::FuncPtrDecl* fp = llvm::dyn_cast<Types::FuncPtrDecl>(def->Type());
	    assert(fp && "Expected function pointer here...");
	    proto = fp->Proto();
	    expr = new VariableExprAST(CurrentToken().Loc(), funcName, def->Type());
	}
    }
    if (expr)
    {
	if (proto->HasSelf())
	{
	    assert(self && "Should have a 'self' expression here");
	    args.insert(args.begin(), self);
	}
	if (FunctionAST* fn = proto->Function())
	{
	    for(auto u : fn->UsedVars())
	    {
		args.push_back(new VariableExprAST(CurrentToken().Loc(), u.Name(), u.Type()));
	    }
	}
	return new CallExprAST(CurrentToken().Loc(), expr, args, proto);
    }
    return 0;
}

ExprAST* Parser::ParseFieldExpr(VariableExprAST* expr, Types::TypeDecl*& type)
{
    if(!Expect(Token::Period, true))
    {
	return 0;
    }
    if (!Expect(Token::Identifier, false))
    {
	return 0;
    }

    std::string typedesc;
    std::string name = CurrentToken().GetIdentName();
    VariableExprAST* e = 0;
    Types::VariantDecl* v = 0;
    unsigned fc = 0;
    if (Types::ObjectDecl* od = llvm::dyn_cast<Types::ObjectDecl>(type))
    {
	int elem = od->Element(name);
	typedesc = "object";
	if (elem >= 0)
	{
	    type = od->GetElement(elem)->FieldType();
	    e = new FieldExprAST(CurrentToken().Loc(), expr, elem, type);
	}
	else 
	{
	    elem = od->MembFunc(name);
	    if (elem >= 0)
	    {
		std::string objname;
		Types::MemberFuncDecl* membfunc = od->GetMembFunc(elem, objname);
		(void) membfunc; // Will be needed later when we do virtual functions!
		std::string funcName = objname + "$" + name;
		NamedObject* def = nameStack.Find(funcName);

		const FuncDef *funcDef = llvm::dyn_cast_or_null<const FuncDef>(def);
		std::vector<ExprAST* > args;
		if (!ParseArgs(funcDef, args))
		{
		    return 0;
		}

		if (ExprAST* call = MakeCallExpr(expr, def, funcName, args))
		{
		    NextToken();
		    return call;
		}
		return 0;
	    }
	    else
	    {
		fc = od->FieldCount();
		v = od->Variant();
	    }
	}
    }
    else if (Types::RecordDecl* rd = llvm::dyn_cast<Types::RecordDecl>(type))
    {
	typedesc = "record";
	int elem = rd->Element(name);
	if (elem >= 0)
	{
	    type = rd->GetElement(elem)->FieldType();
	    e = new FieldExprAST(CurrentToken().Loc(), expr, elem, type);
	}
	else
	{
	    fc = rd->FieldCount();
	    v = rd->Variant();
	}
    }
    else
    {
	return ErrorV("Attempt to use filed of variable that hasn't got fields");
    }
    if (!e && v)
    {
	int elem = v->Element(name);
	if (elem >= 0)
	{
	    const Types::FieldDecl* fd = v->GetElement(elem);
	    type = fd->FieldType();
	    e = new VariantFieldExprAST(CurrentToken().Loc(), expr, fc, type);
	    // If name is empty, we have a made up struct. Dig another level down. 
	    if (fd->Name() == "")
	    {
		Types::RecordDecl* r = llvm::dyn_cast<Types::RecordDecl>(fd->FieldType()); 
		assert(r && "Expect record declarataion");
		elem = r->Element(name);
		if (elem >= 0)
		{
		    type = r->GetElement(elem)->FieldType();
		    e = new FieldExprAST(CurrentToken().Loc(), e, elem, type);
		}
		else
		{
		    e = 0;
		}
	    }
	}
    }
    if (!e)
    {
	return ErrorV(std::string("Can't find element ") + name + " in " + typedesc);
    }
    NextToken();
    return e;
}

VariableExprAST* Parser::ParsePointerExpr(VariableExprAST* expr, Types::TypeDecl*& type)
{
    if (!Expect(Token::Uparrow, true))
    {
	return 0;
    }
    if (type->Type() == Types::File)
    {
	type = type->SubType();
	return new FilePointerExprAST(CurrentToken().Loc(), expr, type);
    }
    type = type->SubType();
    return new PointerExprAST(CurrentToken().Loc(), expr, type);
}

bool Parser::IsCall(Types::TypeDecl* type)
{
    Types::SimpleTypes ty = type->Type();
    if (ty == Types::Pointer &&
	(type->SubType()->Type() == Types::Function ||
	 type->SubType()->Type() == Types::Procedure))
    {
	return true;
    }
    if ((ty == Types::Procedure || ty == Types::Function) && 
	CurrentToken().GetToken() != Token::Assign)
    {
	return true;
    }
    return false;
}

bool Parser::ParseArgs(const FuncDef* funcDef, std::vector<ExprAST*>& args)
{
    TRACE();

    if (CurrentToken().GetToken() == Token::LeftParen)
    {
	// Get past the '(' and fetch the next one. 
	if (!Expect(Token::LeftParen, true))
	{
	    return false;
	}
	unsigned argNo = 0;
	while (CurrentToken().GetToken() != Token::RightParen)
	{
	    bool isFuncArg = false;
	    if (funcDef && funcDef->Proto())
	    {
		auto funcArgs = funcDef->Proto()->Args();
		if (argNo >= funcArgs.size())
		{
		    Error("Too many arguments");
		    return false;
		}
		Types::TypeDecl* td = funcArgs[argNo].Type();
		if (td->Type() == Types::Pointer && 
		    (td->SubType()->Type() == Types::Function ||
		     td->SubType()->Type() == Types::Procedure))
		{
		    isFuncArg = true;
		}
	    }
	    ExprAST* arg;
	    if (isFuncArg)
	    {
		if (CurrentToken().GetToken() != Token::Identifier)
		{
		    Error("Expected name of a function or procedure");
		    return false;
		}
		arg = new FunctionExprAST(CurrentToken().Loc(),
					  CurrentToken().GetIdentName(),
					  funcDef->Proto()->Args()[argNo].Type());
		NextToken();
	    }
	    else
	    {
		arg = ParseExpression();
	    }
	    if (!arg) 
	    {
		return false;
	    }
	    args.push_back(arg);
	    if (CurrentToken().GetToken() == Token::Comma)
	    {
		NextToken();
	    }
	    else if (!Expect(Token::RightParen, false))
	    {
		return false;
	    }
	    argNo++;
	}
	NextToken();
    }
    return true;
}

ExprAST* Parser::ParseIdentifierExpr()
{
    TRACE();

    Token token = CurrentToken();
    TranslateToken(token) ;
    std::string idName = token.GetIdentName();
    NextToken();
    NamedObject* def = nameStack.Find(idName);
    if (EnumDef *enumDef = llvm::dyn_cast_or_null<EnumDef>(def))
    {
	return new IntegerExprAST(token.Loc(), enumDef->Value(), enumDef->Type());
    }
    bool isBuiltin = false;
    if (!def)
    {
	isBuiltin = Builtin::IsBuiltin(idName);
	if (!isBuiltin)
	{
	    return Error(std::string("Undefined name '") + idName + "'");
	}
    }
    if (def)
    {
	// If type is not function, not procedure, or the next thing is an assignment
	// then we want a "variable" with this name. 
	Types::TypeDecl* type = def->Type();
	assert(type && "Expect type here...");
	 
	if (!IsCall(type))
	{
	    VariableExprAST* expr;
	    if (WithDef *w = llvm::dyn_cast<WithDef>(def))
	    {
		expr = llvm::dyn_cast<VariableExprAST>(w->Actual());
	    }
	    else
	    {
		expr = new VariableExprAST(CurrentToken().Loc(), idName, type);
		// Only add defined variables.
		// Ignore result - we may be adding the same variable 
		// several times, but we don't really care.
		usedVariables.Add(idName, def);
	    }
	    assert(expr && "Expected expression here");
	    assert(type && "Type is supposed to be set here");

	    Token::TokenType tt = CurrentToken().GetToken();
	    while(tt == Token::LeftSquare || tt == Token::Uparrow || tt == Token::Period)
	    {
		assert(type && "Expect to have a type here...");
		switch(tt)
		{
		case Token::LeftSquare:
		    expr = ParseArrayExpr(expr, type);
		    break;

		case Token::Uparrow:
		    expr = ParsePointerExpr(expr, type);
		    break;

		case Token::Period:
		    if (ExprAST* tmp = ParseFieldExpr(expr, type))
		    {
			if (VariableExprAST* v = llvm::dyn_cast<VariableExprAST>(tmp))
			{
			    expr = v;
			}
			else
			{
			    return tmp;
			}
		    }
		    break;

		default:
		    assert(0);
		}
		tt = CurrentToken().GetToken();
	    }
	    return expr;	
	}
    }

    std::vector<ExprAST* > args;
    if (!ParseArgs(llvm::dyn_cast_or_null<const FuncDef>(def), args))
    {
	return 0;
    }

    if (ExprAST* expr = MakeCallExpr(NULL, def, idName, args))
    {
	return expr;
    }

    assert(isBuiltin && "Should be a builtin function if we get here");

    if (Builtin::BuiltinFunctionBase* bif = Builtin::CreateBuiltinFunction(idName, args))
    {
	return new BuiltinExprAST(CurrentToken().Loc(), bif);
    }
    assert(0 && "Should not get here");
    return 0;
}

ExprAST* Parser::ParseParenExpr()
{
    NextToken();
    if (ExprAST* v = ParseExpression())
    {
	if (Expect(Token::RightParen, true))
	{
	    return v;
	}
    }
    return 0;
}

ExprAST* Parser::ParseSetExpr()
{
    if (!Expect(Token::LeftSquare, true))
    {
	return 0;
    }

    Location loc = CurrentToken().Loc();
    std::vector<ExprAST*> values;
    do
    {
	if (CurrentToken().GetToken() != Token::RightSquare)
	{
	    ExprAST* v = ParseExpression();
	    if (!v)
	    {
		return 0;
	    }
	    if (CurrentToken().GetToken() == Token::DotDot)
	    {
		NextToken();
		ExprAST* vEnd = ParseExpression();
		v = new RangeExprAST(loc, v, vEnd);
	    }
	    values.push_back(v);
	}
	if (CurrentToken().GetToken() != Token::RightSquare)
	{
	    if (!Expect(Token::Comma, true))
	    {
		return 0;
	    }
	}
    } while(CurrentToken().GetToken() != Token::RightSquare);
    if (!Expect(Token::RightSquare, true))
    {
	return 0;
    }
    Types::TypeDecl* type = NULL;
    if (!values.empty())
    {
	type = values[0]->Type();
    }
    return new SetExprAST(loc, values, new Types::SetDecl(NULL, type));
}

VarDeclAST* Parser::ParseVarDecls()
{
    if (!Expect(Token::Var, true))
    {
	return 0;
    }

    std::vector<VarDef> varList;
    std::vector<std::string> names;
    do
    {
	// Don't move forward here.
	if (!Expect(Token::Identifier, false))
	{
	    return 0;
	}
	names.push_back(CurrentToken().GetIdentName());
	NextToken();
	if (CurrentToken().GetToken() == Token::Colon)
	{
	    NextToken(); 
	    if (Types::TypeDecl* type = ParseType(""))
	    {
		for(auto n : names)
		{
		    VarDef v(n, type);
		    varList.push_back(v);
		    if (!nameStack.Add(n, new VarDef(n, type)))
		    {
			Error(std::string("Name ") + n + " is already defined");
		    }
		}
		if (!Expect(Token::Semicolon, true))
		{
		    return 0;
		}
		names.clear();
	    }
	    else
	    {
		return 0;
	    }
	}
	else
	{
	    if (!Expect(Token::Comma, true))
	    {
		return 0;
	    }
	}
    } while(CurrentToken().GetToken() == Token::Identifier);

    return new VarDeclAST(CurrentToken().Loc(), varList);
}

// functon name( { [var] name1, [,name2 ...]: type [; ...] } ) : type
// procedure name ( { [var] name1 [,name2 ...]: type [; ...] } )
// member function/procedure:
//   function classname.name{( args... )}: type;
//   procedure classname.name{ args... }
PrototypeAST* Parser::ParsePrototype()
{
    assert(CurrentToken().GetToken() == Token::Procedure ||
	   CurrentToken().GetToken() == Token::Function && 
	   "Expected function or procedure token");

    bool isFunction = CurrentToken().GetToken() == Token::Function;

    // Consume "function" or "procedure"
    NextToken();
    if (!Expect(Token::Identifier, false))
    {
	return 0;
    }
    Types::ObjectDecl* od = 0;
    // Get function name.
    std::string funcName = CurrentToken().GetIdentName();
    if (!Expect(Token::Identifier, true))
    {
	return 0;
    }

    Types::MemberFuncDecl* membfunc = 0;
    // Is it a member function?
    // FIXME: Nested classes, should we do this again?
    if (CurrentToken().GetToken() == Token::Period)
    {
	NextToken();
	if (Types::TypeDecl* ty = GetTypeDecl(funcName))
	{
	    if ((od = llvm::dyn_cast<Types::ObjectDecl>(ty)))
	    {
		if (!Expect(Token::Identifier, false))
		{
		    return 0;
		}
		std::string m = CurrentToken().GetIdentName();
		std::string objname;
		int elem;

		if ((elem = od->MembFunc(m)) >= 0)
		{
		    membfunc = od->GetMembFunc(elem, objname);
		}
		if (!membfunc || funcName != objname)
		{
		    Error(std::string("Member function '") + m + "' not found in '"  + funcName + "'.");
		    return 0;
		}
		funcName = funcName + "$" + m;
		NextToken();
	    }
	}
	if (!od)
	{
	    Error("Expected object name");
	    return 0;
	}
    }
    std::vector<VarDef> args;
    if (CurrentToken().GetToken() == Token::LeftParen)
    {
	std::vector<std::string> names;
	NextToken();
	bool isRef = false;
	while(CurrentToken().GetToken() != Token::RightParen)
	{
	    if (CurrentToken().GetToken() == Token::Function ||
		CurrentToken().GetToken() == Token::Procedure)
	    {
		PrototypeAST* proto = ParsePrototype();
		Types::TypeDecl* type = new Types::FuncPtrDecl(proto);
		VarDef v(proto->Name(), type, false);
		args.push_back(v);
	    }
	    else
	    {
		if (CurrentToken().GetToken() == Token::Var)
		{
		    isRef = true;
		    NextToken();
		}
		if (!Expect(Token::Identifier, false))
		{
		    return 0;
		}
		std::string arg = CurrentToken().GetIdentName();
		NextToken();

		names.push_back(arg);
		if (CurrentToken().GetToken() == Token::Colon)
		{
		    NextToken();
		    if (Types::TypeDecl* type = ParseType(""))
		    {
			for(auto n : names)
			{
			    VarDef v(n, type, isRef);
			    args.push_back(v);
			}
			isRef = false;
			names.clear();
			if (CurrentToken().GetToken() != Token::RightParen)
			{
			    if (!Expect(Token::Semicolon, true))
			    {
				return 0;
			    }
			}
		    }
		    else
		    {
			return 0;
		    }
		}
		else
		{
		    if (!Expect(Token::Comma, true))
		    {
			return 0;
		    }
		}
	    }
	}
	// Eat ')' at end of argument list.
	if (!Expect(Token::RightParen, true))
	{
	    return 0;
	}
    }

    PrototypeAST* proto = 0;
    Types::TypeDecl* resultType;
    // If we have a function, expect ": type".
    if (isFunction)
    {
	if (!Expect(Token::Colon, true))
	{
	    return 0;
	}
        resultType = ParseSimpleType();
	if (!resultType)
	{
	    return 0;
	}
    }
    else
    {
	resultType = Types::GetVoidType();
    }
    
    if (!Expect(Token::Semicolon, true))
    {
	return 0;
    }

    proto = new PrototypeAST(CurrentToken().Loc(), funcName, args, resultType, od);
    if (od && !membfunc->IsStatic())
    {
	std::vector<VarDef> v{VarDef("self", od, true)};
	proto->AddExtraArgsFirst(v);
	proto->SetHasSelf(true);
	
    }
    return proto;
}

ExprAST* Parser::ParseStatement()
{
    if (ExprAST* expr = ParsePrimary())
    {
	if (CurrentToken().GetToken() == Token::Assign)
	{
	    Location loc = CurrentToken().Loc();
	    NextToken();
	    ExprAST* rhs = ParseExpression();
	    if (rhs)
	    {
		expr = new AssignExprAST(loc, expr, rhs);
	    }
	    else
	    {
		// TODO: Error recovery.
		return 0;
	    }
	}
	return expr;
    }
    // TODO: Error recovery.
    return 0;
}

BlockAST* Parser::ParseBlock()
{
    if (!Expect(Token::Begin, true))
    {
	return 0;
    }

    std::vector<ExprAST*> v;
    // Build ast of the content of the block.
    Location loc = CurrentToken().Loc();
    while(CurrentToken().GetToken() != Token::End)
    {
	if (ExprAST* ast = ParseStatement())
	{
	    v.push_back(ast);
	    if (!ExpectSemicolonOrEnd())
	    {
		return 0;
	    }
	}
	else
	{
	    return 0;
	}
    }
    if (!Expect(Token::End, true))
    {
	return 0;
    }
    return new BlockAST(loc, v);
}

FunctionAST* Parser::ParseDefinition(int level)
{
    Location loc = CurrentToken().Loc();
    Types::SimpleTypes functionType = 
	CurrentToken().GetToken() == Token::Function?Types::Function:Types::Procedure;
    PrototypeAST* proto = ParsePrototype();
    if (!proto)
    {
	return 0;
    }
    std::string      name = proto->Name();
    Types::TypeDecl* ty = new Types::FunctionDecl(functionType, proto->Type());
    NamedObject*     nmObj = new FuncDef(name, ty, proto);

    const NamedObject* def = nameStack.Find(name);
    const FuncDef *fnDef = llvm::dyn_cast_or_null<const FuncDef>(def);
    if (!(fnDef && fnDef->Proto() && fnDef->Proto()->IsForward()))
    {
	if (!nameStack.Add(name, nmObj))
	{
	    return ErrorF(std::string("Name '") + name + "' already exists...");
	}
	
	if (CurrentToken().GetToken() == Token::Forward)
	{
	    NextToken();
	    proto->SetIsForward(true);
	    return new FunctionAST(CurrentToken().Loc(), proto, 0, 0);
	}
    }

    NameWrapper wrapper(nameStack);
    NameWrapper usedWrapper(usedVariables);
    for(auto v : proto->Args())
    {
	if (!nameStack.Add(v.Name(), new VarDef(v.Name(), v.Type())))
	{
	    return ErrorF(std::string("Duplicate name ") + v.Name()); 
	}
    }
    if (proto->HasSelf())
    {
	VariableExprAST* v = new VariableExprAST(Location("", 0, 0), "self", proto->BaseObj());
	ExpandWithNames(proto->BaseObj(), v, 0);
    }

    VarDeclAST*               varDecls = 0;
    BlockAST*                 body = 0;
    bool                      typeDecls = false;
    bool                      constDecls = false;
    std::vector<FunctionAST*> subFunctions;
    do
    {
	switch(CurrentToken().GetToken())
	{
	case Token::Var:
	    if (varDecls)
	    {
		return ErrorF("Can't declare variables multiple times");
	    }
	    varDecls = ParseVarDecls();
	    break;

	case Token::Type:
	    if (typeDecls)
	    {
		return ErrorF("Can't declare types multiple times");
	    }
	    ParseTypeDef();
	    typeDecls = true;
	    break;

	case Token::Const:
	    if (constDecls)
	    {
		return ErrorF("Can't declare const multiple times");
	    }
	    ParseConstDef();
	    constDecls = true;
	    break;

	case Token::Function:
	case Token::Procedure:
	{
	    FunctionAST* fn = ParseDefinition(level+1);
	    assert(fn && "Expected to get a function definition");
	    if (fn)
	    {
		subFunctions.push_back(fn);
	    }
	    break;
	}
	   
	case Token::Begin:
	{
	    if (body)
	    {
		return ErrorF("Multiple body declarations for function?");
	    }
	    body = ParseBlock();
	    if (!body)
	    {
		return 0;
	    }
	    if (!Expect(Token::Semicolon, true))
	    {
		return 0;
	    }

	    FunctionAST* fn = new FunctionAST(loc, proto, varDecls, body);
	    for(auto s : subFunctions)
	    {
		s->SetParent(fn);
	    }
	    // Need to add subFunctions before setting used vars!
	    fn->AddSubFunctions(subFunctions);
	    fn->SetUsedVars(usedVariables.GetLevel(), nameStack);
	    proto->AddExtraArgsLast(fn->UsedVars());
	    UpdateCallVisitor updater(proto);
	    fn->accept(updater);
	    return fn;
	}

	default:
	    assert(0 && "Unexpected token");
	    return ErrorF("Unexpected token");
	}
    } while(1);
    return 0;
}

ExprAST* Parser::ParseStmtOrBlock()
{
    switch(CurrentToken().GetToken())
    {
    case Token::Begin:
	return ParseBlock();

    case Token::Semicolon:
    case Token::End:
	// Empty block.
	return new BlockAST(CurrentToken().Loc(), std::vector<ExprAST*>());

    default:
	return ParseStatement();
    }
}

ExprAST* Parser::ParseIfExpr()
{
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::If, true))
    {
	assert(0 && "Huh? Expected if");
    }
    ExprAST* cond = ParseExpression();
    if (!cond || !Expect(Token::Then, true))
    {
	return 0;
    }

    ExprAST* then = 0;
    if (CurrentToken().GetToken() != Token::Else)
    {
	then = ParseStmtOrBlock();
	if (!then)
	{
	    return 0;
	}
    }

    ExprAST* elseExpr = 0;
    if (CurrentToken().GetToken() == Token::Else)
    {
	if (Expect(Token::Else, true))
	{
	    if (!(elseExpr = ParseStmtOrBlock()))
	    {
		return 0;
	    }
	}
    }
    return new IfExprAST(loc, cond, then, elseExpr);
}

ExprAST* Parser::ParseForExpr()
{
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::For, true))
    {
	return 0;
    }
    if (CurrentToken().GetToken() != Token::Identifier)
    {
	return Error("Expected identifier name, got " + CurrentToken().ToString());
    }
    std::string varName = CurrentToken().GetIdentName();
    NextToken();
    if (!Expect(Token::Assign, true))
    {
	return 0;
    }
    ExprAST* start = ParseExpression();
    if (!start)
    {
	return 0;
    }
    bool down = false;
    Token::TokenType tt = CurrentToken().GetToken();
    if (tt == Token::Downto || tt == Token::To)
    {
	down = (tt == Token::Downto);
	NextToken();
    }
    else
    {
	return Error("Expected 'to' or 'downto', got " + CurrentToken().ToString());
    }

    ExprAST* end = ParseExpression();
    if (!end || !Expect(Token::Do, true))
    {
	return 0;
    }
    if (ExprAST* body = ParseStmtOrBlock())
    {
	return new ForExprAST(loc, varName, start, end, down, body);
    }
    return 0;
}

ExprAST* Parser::ParseWhile()
{
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::While, true))
    {
	return 0;
    }

    ExprAST* cond = ParseExpression();
    if (!cond || !Expect(Token::Do, true))
    {
	return 0;
    }
    ExprAST* body = ParseStmtOrBlock();

    return new WhileExprAST(loc, cond, body);
}

ExprAST* Parser::ParseRepeat()
{
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::Repeat, true))
    {
	return 0;
    }
    std::vector<ExprAST*> v;
    Location loc2 = CurrentToken().Loc();
    while(CurrentToken().GetToken() != Token::Until)
    {
	if (ExprAST* stmt = ParseStatement())
	{
	    v.push_back(stmt);
	    if(CurrentToken().GetToken() == Token::Semicolon)
	    {
		NextToken();
	    }
	}
	else
	{
	    return 0;
	}
    }
    if (!Expect(Token::Until, true))
    {
	return 0;
    }
    ExprAST* cond = ParseExpression();
    return new RepeatExprAST(loc, cond, new BlockAST(loc2, v));
}

ExprAST* Parser::ParseCaseExpr()
{
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::Case, true))
    {
	return 0;
    }
    ExprAST* expr = ParseExpression();
    if (!Expect(Token::Of, true))
    {
	return 0;
    }
    std::vector<LabelExprAST*> labels;
    std::vector<int> lab;
    bool isFirst = true;
    Token::TokenType prevTT;
    ExprAST* otherwise = 0;
    do
    {
	bool isOtherwise = false;
	if (isFirst)
	{
	    prevTT = CurrentToken().GetToken();
	    isFirst = false;
	}
	else if (CurrentToken().GetToken() != Token::Otherwise && 
		 prevTT != CurrentToken().GetToken())
	{
	    return Error("Type of case labels must not change type");
	}
	switch(CurrentToken().GetToken())
	{
	case Token::Char:
	case Token::Integer:
	    lab.push_back(CurrentToken().GetIntVal());
	    break;
	    
	case Token::Identifier:
	{
	    EnumDef* ed = GetEnumValue(CurrentToken().GetIdentName());
	    if (ed)
	    {
		lab.push_back(ed->Value());
	    }
	    else
	    {
		return Error("Expected enumerated type value");
	    }
	    break;
	}

	case Token::Otherwise:
	    if (otherwise)
	    {
		return Error("Otherwise already used in this case block");
	    }
	    isOtherwise = true;
	    break;

	default:
	    return Error("Syntax error, expected case label");
	}
	NextToken();
	switch(CurrentToken().GetToken())
	{
	case Token::Comma:
	    if (isOtherwise)
	    {
		return Error("Can't have multiple case labels with otherwise case label");
	    }
	    NextToken();
	    break;

	case Token::Colon:
	{
	    Location locColon = CurrentToken().Loc();
	    NextToken();
	    ExprAST* s = ParseStmtOrBlock();
	    if (isOtherwise)
	    {
		otherwise = s;
		if (lab.size())
		{
		    return Error("Can't have multiple case labels with otherwise case label");
		}
	    }
	    else
	    {
		labels.push_back(new LabelExprAST(locColon, lab, s));
		lab.clear();
	    }
	    if (!ExpectSemicolonOrEnd())
	    {
		return 0;
	    }
	    break;
	}
	
	default:
	    return Error("Syntax error: Expected ',' or ':' in case-statement.");
	}
    } while(CurrentToken().GetToken() != Token::End);
    if (!Expect(Token::End, true))
    {
	return 0;
    }
    return new CaseExprAST(loc, expr, labels, otherwise);
}

void Parser::ExpandWithNames(const Types::FieldCollection* fields, VariableExprAST* v, int parentCount)
{
    TRACE();
    int count = fields->FieldCount();
    for(int i = 0; i < count; i++)
    {
	const Types::FieldDecl *f = fields->GetElement(i);
	Types::TypeDecl* ty = f->FieldType();
	if (f->Name() == "")
	{
	    Types::RecordDecl* rd = llvm::dyn_cast<Types::RecordDecl>(ty);
	    assert(rd && "Expected record declarataion here!");
	    ExpandWithNames(rd, new VariantFieldExprAST(CurrentToken().Loc(), v, parentCount, ty), 0);
	}
	else
	{
	    ExprAST* e;
	    if (llvm::isa<Types::RecordDecl>(fields) || llvm::isa<Types::ObjectDecl>(fields))
	    {
		e = new FieldExprAST(CurrentToken().Loc(), v, i, ty);
	    }
	    else
	    {
		e = new VariantFieldExprAST(CurrentToken().Loc(), v, parentCount, ty);
	    }
	    nameStack.Add(f->Name(), new WithDef(f->Name(), e, f->FieldType()));
	}
    }
}

ExprAST* Parser::ParseWithBlock()
{
    TRACE();
    Location loc = CurrentToken().Loc();
    if (!Expect(Token::With, true))
    {
	return 0;
    }
    std::vector<VariableExprAST*> vars;
    do
    {
	ExprAST* e = ParseIdentifierExpr();
	if (VariableExprAST* v = llvm::dyn_cast_or_null<VariableExprAST>(e))
	{
	    vars.push_back(v);
	    if (CurrentToken().GetToken() != Token::Do)
	    {
		if (!Expect(Token::Comma, true))
		{
		    return 0;
		}
	    }
	}
	else
	{
	    return Error("With statement must contain only variable expression");
	}
    } while(CurrentToken().GetToken() != Token::Do);
    if (!Expect(Token::Do, true))
    {
	return 0;
    }

    NameWrapper wrapper(nameStack);
    for(auto v : vars)
    {
	if(Types::RecordDecl* rd = llvm::dyn_cast<Types::RecordDecl>(v->Type()))
	{
	    ExpandWithNames(rd, v, 0);
	    if (Types::VariantDecl* variant = rd->Variant())
	    {
		ExpandWithNames(variant, v, rd->FieldCount());
	    }
	}
	else
	{
	    return Error("Type for with statement should be a record type");
	}
    }
    ExprAST* body = ParseStmtOrBlock();
    return new WithExprAST(loc, body);
}

ExprAST* Parser::ParseWrite()
{
    Location loc = CurrentToken().Loc();
    bool isWriteln = CurrentToken().GetToken() == Token::Writeln;

    assert(CurrentToken().GetToken() == Token::Write ||
	   CurrentToken().GetToken() == Token::Writeln &&
	   "Expected write or writeln keyword here");
    NextToken();

    VariableExprAST* file = 0;
    std::vector<WriteAST::WriteArg> args;
    if (CurrentToken().GetToken() == Token::Semicolon || CurrentToken().GetToken() == Token::End)
    {
	if (!isWriteln)
	{
	    return Error("Write must have arguments.");
	}
	else
	{
	    file = new VariableExprAST(loc, "output", Types::GetTextType());
	}
    }
    else
    {
	if (!Expect(Token::LeftParen, true))
	{
	    return 0;
	}
	
	while(CurrentToken().GetToken() != Token::RightParen)
	{
	    WriteAST::WriteArg wa;
	    wa.expr = ParseExpression();
	    if (!wa.expr)
	    {
		return 0;
	    }
	    if (args.size() == 0)
	    {
		if (VariableExprAST* vexpr = llvm::dyn_cast<VariableExprAST>(wa.expr))
		{
		    if (vexpr->Type()->Type() == Types::File)
		    {
			file = vexpr;
			wa.expr = 0;
		    }
		}
		if (file == 0)
		{
			file = new VariableExprAST(loc, "output", Types::GetTextType());
		}
	    }
	    if (wa.expr)
	    {
		if (CurrentToken().GetToken() == Token::Colon)
		{
		    NextToken();
		    wa.width = ParseExpression();
		    if (!wa.width)
		    {
			return Error("Invalid width expression");
		    }
		}
		if (CurrentToken().GetToken() == Token::Colon)
		{
		    NextToken();
		    wa.precision = ParseExpression();
		    if (!wa.precision)
		    {
			return Error("Invalid precision expression");
		    }
		}
		args.push_back(wa);
	    }
	    if (CurrentToken().GetToken() != Token::RightParen)
	    {
		if (!Expect(Token::Comma, true))
		{
		    return 0;
		}
	    }
	}
	if(!Expect(Token::RightParen, true))
	{
	    return 0;
	}
	if (args.size() < 1 && !isWriteln)
	{
	    return Error("Expected at least one expression for output in write");
	}
    }
    return new WriteAST(loc, file, args, isWriteln);
}

ExprAST* Parser::ParseRead()
{
    Location loc = CurrentToken().Loc();
    bool isReadln = CurrentToken().GetToken() == Token::Readln;

    assert(CurrentToken().GetToken() == Token::Read ||
	   CurrentToken().GetToken() == Token::Readln &&
	   "Expected read or readln keyword here");
    NextToken();

    std::vector<ExprAST*> args;
    VariableExprAST* file = 0;
    if (CurrentToken().GetToken() == Token::Semicolon || CurrentToken().GetToken() == Token::End)
    {
	if (!isReadln)
	{
	    return Error("Read must have arguments.");
	}
	else
	{
	    file = new VariableExprAST(loc, "input", Types::GetTextType());
	}
    }
    else
    {
	if (!Expect(Token::LeftParen, true))
	{
	    return 0;
	}
	while(CurrentToken().GetToken() != Token::RightParen)
	{
	    ExprAST* expr = ParseExpression();
	    if (!expr)
	    {
		return 0;
	    }
	    if (args.size() == 0)
	    {
		if (VariableExprAST* vexpr = llvm::dyn_cast<VariableExprAST>(expr))
		{
		    if (vexpr->Type()->Type() == Types::File)
		    {
			file = vexpr;
			expr = 0;
		    }
		}
		if (file == 0)
		{
		    file = new VariableExprAST(loc, "input", Types::GetTextType());
		}
	    }
	    if (expr)
	    {
		args.push_back(expr);
	    }
	    if (CurrentToken().GetToken() != Token::RightParen)
	    {
		if (!Expect(Token::Comma, true))
		{
		    return 0;
		}
	    }
	}
	if(!Expect(Token::RightParen, true))
	{
	    return 0;
	}
	if (args.size() < 1 && !isReadln)
	{
	    return Error("Expected at least one variable in read statement");
	}
    }
    return new ReadAST(loc, file, args, isReadln);
}

ExprAST* Parser::ParsePrimary()
{
    Token token = CurrentToken();
    TranslateToken(token);

    switch(token.GetToken())
    {
    case Token::Nil:
	return ParseNilExpr();

    case Token::Real:
	return ParseRealExpr(token);

    case Token::Integer:
	return ParseIntegerExpr(token);

    case Token::Char:
	return ParseCharExpr(token);

    case Token::StringLiteral:
	return ParseStringExpr(token);

    case Token::LeftParen:
	return ParseParenExpr();

    case Token::LeftSquare:
	return ParseSetExpr();
	
    case Token::Identifier:
	return ParseIdentifierExpr();

    case Token::If:
	return ParseIfExpr();

    case Token::For:
	return ParseForExpr();

    case Token::While:
	return ParseWhile();

    case Token::Repeat:
	return ParseRepeat();

    case Token::Case:
	return ParseCaseExpr();

    case Token::With:
	return ParseWithBlock();

    case Token::Write:
    case Token::Writeln:
	return ParseWrite();

    case Token::Read:
    case Token::Readln:
	return ParseRead();

    case Token::Minus:
    case Token::Plus:
    case Token::Not:
	return ParseUnaryOp();

    case Token::SizeOf:
	return ParseSizeOfExpr();

    default:
	CurrentToken().dump(std::cerr);
	assert(0 && "Unexpected token");
	return 0;
    }
}

bool Parser::ParseProgram()
{
    if (!Expect(Token::Program, true))
    {
	return false;
    }
    if (!Expect(Token::Identifier, false))
    {
	return false;
    }
    moduleName = CurrentToken().GetIdentName();
    NextToken();
    if (CurrentToken().GetToken() == Token::LeftParen)
    {
	NextToken();
	do
	{
	    if (!Expect(Token::Identifier, true))
	    {
		return false;
	    }
	    if (CurrentToken().GetToken() != Token::RightParen)
	    {
		if (!Expect(Token::Comma, true))
		{
		    return false;
		}
	    }
	} while(CurrentToken().GetToken() != Token::RightParen);
	NextToken();
    }
    return true;
}

std::vector<ExprAST*> Parser::Parse()
{
    TIME_TRACE();
    std::vector<ExprAST*> v;

    NextToken();
    if(!ParseProgram())
    {
	return v;
    }

    VarDef input("input", Types::GetTextType(), false, true);
    VarDef output("output", Types::GetTextType(), false, true);
    nameStack.Add("input", new VarDef(input));
    nameStack.Add("output", new VarDef(output));
    std::vector<VarDef> varList{input, output};

    v.push_back(new VarDeclAST(Location("", 0, 0), varList));

    for(;;)
    {
	ExprAST* curAst = 0;
	switch(CurrentToken().GetToken())
	{
	case Token::EndOfFile:
	    // TODO: Is this not an error?
	    return v;
	    
	case Token::Semicolon:
	    NextToken();
	    break;

	case Token::Function:
	case Token::Procedure:
	    curAst = ParseDefinition(0);
	    break;

	case Token::Var:
	    curAst = ParseVarDecls();
	    break;

	case Token::Type:
	    ParseTypeDef();   
	    // Generates no AST, so need to "continue" here
	    continue;

	case Token::Const:
	    ParseConstDef();
	    // No AST from constdef, so continue.
	    continue;
	    
	case Token::Begin:
	{
	    Location loc = CurrentToken().Loc();
	    BlockAST* body = ParseBlock();
	    // Parse the "main" of the program - we call that
	    // "__PascalMain" so we can call it from C-code.
	    PrototypeAST* proto = new PrototypeAST(loc, "__PascalMain", std::vector<VarDef>());
	    FunctionAST* fun = new FunctionAST(loc, proto, 0, body);
	    curAst = fun;
	    if (!Expect(Token::Period, true))
	    {
		v.clear();
		return v;
	    }
	    break;
	}

	default:
	    curAst = ParseExpression();
	    break;
	}

	if (curAst)
	{
	    v.push_back(curAst);
	}
    }
}
