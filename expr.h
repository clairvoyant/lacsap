#ifndef EXPR_H
#define EXPR_H

#include "token.h"
#include "namedobject.h"
#include "types.h"
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/PassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <string>
#include <vector>
#include <iostream>

extern llvm::FunctionPassManager* fpm;
extern llvm::Module* theModule;



class ExprAST
{
public:
    enum ExprKind
    {
	EK_Expr,
	EK_RealExpr,
	EK_IntegerExpr,
	EK_CharExpr,
	EK_StringExpr,

	// Addressable types
	EK_AddressableExpr,
	EK_VariableExpr,
	EK_ArrayExpr,
	EK_PointerExpr,
	EK_FilePointerExpr,
	EK_FieldExpr,
	EK_FunctionExpr,
	EK_SetExpr,
	EK_LastAddressable,
    
	EK_BinaryExpr,
	EK_UnaryExpr,
	EK_RangeExpr,
	EK_Block,
	EK_AssignExpr,
	EK_VarDecl,
	EK_Function, 
	EK_Prototype, 
	EK_CallExpr,
	EK_BuiltinExpr,
	EK_IfExpr,
	EK_ForExpr,
	EK_WhileExpr,
	EK_RepeatExpr,
	EK_Write,
	EK_Read,
	EK_LabelExpr,
	EK_CaseExpr,
    };
    ExprAST(ExprKind k) : kind(k) {}
    virtual ~ExprAST() {}
    void Dump(std::ostream& out) const;
    void Dump() const;
    virtual void DoDump(std::ostream& out) const
    { 
	out << "Empty node";
    }
    std::string ToString();
    virtual llvm::Value* CodeGen() = 0;
    ExprKind getKind() const { return kind; }
private:
    const ExprKind kind;
};

class RealExprAST : public ExprAST
{
public:
    RealExprAST(double v) 
	: ExprAST(EK_RealExpr), val(v) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_RealExpr; }
private:
    double val;
};

class IntegerExprAST : public ExprAST
{
public:
    IntegerExprAST(int v, llvm::Type* ty = 0) 
	: ExprAST(EK_IntegerExpr), val(v), type(ty) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_IntegerExpr; }
private:
    int         val;
    llvm::Type* type;
};

class CharExprAST : public ExprAST
{
public:
    CharExprAST(char v) 
	: ExprAST(EK_CharExpr), val(v) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_CharExpr; }
private:
    char val;
};

class StringExprAST : public ExprAST
{
public:
    StringExprAST(const std::string &v) 
	: ExprAST(EK_StringExpr), val(v) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_StringExpr; }
private:
    std::string val;
};


class AddressableAST : public ExprAST
{
public:
    AddressableAST(ExprKind k) :
	ExprAST(k) {}
    virtual llvm::Value* Address() = 0;
    static bool classof(const ExprAST *e) 
    {
	return e->getKind() >= EK_AddressableExpr && 
	    e->getKind() <= EK_LastAddressable; 
    }
};

class VariableExprAST : public AddressableAST
{
public:
    VariableExprAST(const std::string& nm, Types::TypeDecl* ty) 
	: AddressableAST(EK_VariableExpr), name(nm), type(ty) {}
    VariableExprAST(ExprKind k, const std::string& nm, Types::TypeDecl* ty) 
	: AddressableAST(k), name(nm), type(ty) {}
    VariableExprAST(ExprKind k, const VariableExprAST* v, Types::TypeDecl* ty) 
	: AddressableAST(k), name(v->name), type(ty) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    const std::string& Name() const { return name; }
    virtual llvm::Value* Address();
    Types::TypeDecl *Type() const { return type; }
    static bool classof(const ExprAST *e) 
    { 
	return e->getKind() >= EK_VariableExpr && 
	    e->getKind() <= EK_LastAddressable; 
    }
protected:
    std::string name;
    Types::TypeDecl* type;
};

class ArrayExprAST : public VariableExprAST
{
public:
    ArrayExprAST(VariableExprAST *v,
		 const std::vector<ExprAST*>& inds, 
		 const std::vector<Types::Range*>& r, 
		 Types::TypeDecl* ty)
	: VariableExprAST(EK_ArrayExpr, v, ty), expr(v), indices(inds), ranges(r)
    {
	size_t mul = 1;
	for(auto j = ranges.end()-1; j >= ranges.begin(); j--)
	{
	    indexmul.push_back(mul);
	    mul *= (*j)->Size();
	}
	std::reverse(indexmul.begin(), indexmul.end());
    }
    virtual void DoDump(std::ostream& out) const;
    /* Don't need CodeGen, just calculate address and use parent CodeGen */
    virtual llvm::Value* Address();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_ArrayExpr; }
private:
    VariableExprAST* expr;
    std::vector<ExprAST*> indices;
    std::vector<Types::Range*> ranges;
    std::vector<size_t> indexmul;
};

class PointerExprAST : public VariableExprAST
{
public:
    PointerExprAST(VariableExprAST *p, Types::TypeDecl* ty)
	: VariableExprAST(EK_PointerExpr, p, ty), pointer(p) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    virtual llvm::Value* Address();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_PointerExpr; }
private:
    ExprAST* pointer;
};

class FilePointerExprAST : public VariableExprAST
{
public:
    FilePointerExprAST(VariableExprAST *p, Types::TypeDecl* ty)
	: VariableExprAST(EK_FilePointerExpr, p, ty), pointer(p) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    virtual llvm::Value* Address();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_FilePointerExpr; }
private:
    ExprAST* pointer;
};

class FieldExprAST : public VariableExprAST
{
public:
    FieldExprAST(VariableExprAST* base, int elem, Types::TypeDecl* ty)
	: VariableExprAST(EK_FieldExpr, base, ty), expr(base), element(elem) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* Address();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_FieldExpr; }
private:
    VariableExprAST* expr;
    int element;
};

class FunctionExprAST : public VariableExprAST
{
public:
    FunctionExprAST(const std::string& nm, Types::TypeDecl* ty)
	: VariableExprAST(EK_FunctionExpr, nm, ty) { }

    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* Address();
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_FunctionExpr; }
};

class SetExprAST : public AddressableAST
{
public:
    SetExprAST(std::vector<ExprAST*> v)
	: AddressableAST(EK_SetExpr), values(v) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    virtual llvm::Value* Address();
private:
    std::vector<ExprAST*> values;
    static bool classof(const ExprAST *e) { return e->getKind() == EK_SetExpr; }
};

class BinaryExprAST : public ExprAST
{
public:
    BinaryExprAST(Token op, ExprAST* l, ExprAST* r)
	: ExprAST(EK_BinaryExpr), oper(op), lhs(l), rhs(r) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_BinaryExpr; }
private:
    Token oper;
    ExprAST* lhs, *rhs;
};

class UnaryExprAST : public ExprAST
{
public:
    UnaryExprAST(Token op, ExprAST* r)
	: ExprAST(EK_UnaryExpr), oper(op), rhs(r) {};
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_UnaryExpr; }
private:
    Token oper;
    ExprAST* rhs;
};

class RangeExprAST : public ExprAST
{
public:
    RangeExprAST(ExprAST* l, ExprAST* h)
	: ExprAST(EK_RangeExpr), low(l), high(h) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    virtual llvm::Value* Low() { return low->CodeGen(); }
    virtual llvm::Value* High() { return high->CodeGen(); }
    static bool classof(const ExprAST *e) { return e->getKind() == EK_RangeExpr; }
private:
    ExprAST* low;
    ExprAST* high;
};

class BlockAST : public ExprAST
{
public:
    BlockAST(std::vector<ExprAST*> block) 
	: ExprAST(EK_Block), content(block) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    bool IsEmpty() { return content.size() == 0; }
    static bool classof(const ExprAST *e) { return e->getKind() == EK_Block; }
private:
    std::vector<ExprAST*> content;
};

class AssignExprAST : public ExprAST
{
public:
    AssignExprAST(ExprAST* l, ExprAST* r)
	: ExprAST(EK_AssignExpr), lhs(l), rhs(r) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_AssignExpr; }
private:
    ExprAST* lhs, *rhs;
};

class VarDeclAST : public ExprAST
{
public:
    VarDeclAST(std::vector<VarDef> v)
	: ExprAST(EK_VarDecl), vars(v), func(0) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    void SetFunction(llvm::Function* f) { func = f; }
    static bool classof(const ExprAST *e) { return e->getKind() == EK_VarDecl; }
private:
    std::vector<VarDef> vars;
    llvm::Function* func;
};

class FunctionAST;

class PrototypeAST : public ExprAST
{
public:
    PrototypeAST(const std::string& nm, const std::vector<VarDef>& ar) 
	: ExprAST(EK_Prototype), name(nm), args(ar), isForward(false), function(0)
    { 
	resultType = new Types::TypeDecl(Types::Void); 
    }
    PrototypeAST(const std::string& nm, const std::vector<VarDef>& ar, Types::TypeDecl* resTy) 
	: ExprAST(EK_Prototype), name(nm), args(ar), resultType(resTy), isForward(false), function(0)
    {
	assert(resTy && "Type must not be null!");
    }
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Function* CodeGen();
    virtual llvm::Function* CodeGen(const std::string& namePrefix);
    void CreateArgumentAlloca(llvm::Function* fn);
    Types::TypeDecl* ResultType() const { return resultType; }
    std::string Name() const { return name; }
    const std::vector<VarDef>& Args() const { return args; }
    bool IsForward() { return isForward; }
    void SetIsForward(bool v) { isForward = v; }
    void SetFunction(FunctionAST* fun) { function = fun; }
    FunctionAST* Function() const { return function; }
    void AddExtraArgs(const std::vector<VarDef>& extra);
    static bool classof(const ExprAST *e) { return e->getKind() == EK_Prototype; }
private:
    std::string         name;
    std::vector<VarDef> args;
    Types::TypeDecl*    resultType;
    bool                isForward;
    FunctionAST*        function;
};

class FunctionAST : public ExprAST
{
public:
    FunctionAST(PrototypeAST *prot, VarDeclAST* v, BlockAST* b) 
	: ExprAST(EK_Function), proto(prot), varDecls(v), body(b)
    { 
	assert((proto->IsForward() || body) && "Function should have body"); 
	if (!proto->IsForward())
	{
	    proto->SetFunction(this);
	}
    }
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Function* CodeGen();
    llvm::Function* CodeGen(const std::string& namePrefix);
    const PrototypeAST* Proto() const { return proto; }
    void AddSubFunctions(const std::vector<FunctionAST *>& subs) { subFunctions = subs; }
    void SetParent(FunctionAST* p) { parent = p; }
    void SetUsedVars(const std::vector<NamedObject*>& varsUsed, 
		     const std::vector<NamedObject*>& localVars,
		     const std::vector<NamedObject*>& globalVars);
    const std::vector<VarDef>& UsedVars() { return usedVariables; }
    static bool classof(const ExprAST *e) { return e->getKind() == EK_Function; }
private:
    PrototypeAST*              proto;
    VarDeclAST*                varDecls;
    BlockAST*                  body;
    std::vector<FunctionAST*>  subFunctions;
    std::vector<VarDef>        usedVariables;
    FunctionAST*               parent;
};

class CallExprAST : public ExprAST
{
public:
    CallExprAST(ExprAST *c, std::vector<ExprAST*> a, const PrototypeAST* p)
	: ExprAST(EK_CallExpr), proto(p), callee(c), args(a) 
    {
	assert(proto && "Should have prototype!");
    }
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_CallExpr; }
private:
    const PrototypeAST*   proto;
    ExprAST*              callee;
    std::vector<ExprAST*> args;
};

// Builtin function call
class BuiltinExprAST : public ExprAST
{
public:
    BuiltinExprAST(const std::string& nm, std::vector<ExprAST*> a)
	: ExprAST(EK_BuiltinExpr), name(nm), args(a) 
    {
    }
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_BuiltinExpr; }
private:
    std::string           name;
    std::vector<ExprAST*> args;
};

class IfExprAST : public ExprAST
{
public:
    IfExprAST(ExprAST* c, ExprAST* t, ExprAST* e) 
	: ExprAST(EK_IfExpr), cond(c), then(t), other(e) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_IfExpr; }
private:
    ExprAST* cond;
    ExprAST* then;
    ExprAST* other;
};

class ForExprAST : public ExprAST
{
public:
    ForExprAST(const std::string& var, ExprAST* s, ExprAST* e, bool down, ExprAST* b)
	: ExprAST(EK_ForExpr), varName(var), start(s), stepDown(down), end(e), body(b) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_ForExpr; }
private:
    std::string varName;
    ExprAST* start;
    bool     stepDown;   // true for "downto" 
    ExprAST* end;
    ExprAST* body;
};

class WhileExprAST : public ExprAST
{
public:
    WhileExprAST(ExprAST* c, ExprAST* b)
	: ExprAST(EK_WhileExpr), cond(c), body(b) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_WhileExpr; }
private:
    ExprAST* cond;
    ExprAST* body;
};

class RepeatExprAST : public ExprAST
{
public:
    RepeatExprAST(ExprAST* c, ExprAST* b)
	: ExprAST(EK_RepeatExpr), cond(c), body(b) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_RepeatExpr; }
private:
    ExprAST* cond;
    ExprAST* body;
};

class WriteAST : public ExprAST
{
public:
    struct WriteArg
    {
	WriteArg() 
	    : expr(0), width(0), precision(0)  {}
	ExprAST* expr;
	ExprAST* width;
	ExprAST* precision;
    };

    WriteAST(VariableExprAST* f, const std::vector<WriteArg> &a, bool isLn)
	: ExprAST(EK_Write), file(f), args(a), isWriteln(isLn) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_Write; }
private:
    VariableExprAST*      file;
    std::vector<WriteArg> args;
    bool                  isWriteln;
};

class ReadAST : public ExprAST
{
public:
    ReadAST(VariableExprAST* fi, const std::vector<ExprAST*> &a, bool isLn)
	: ExprAST(EK_Read), file(fi), args(a), isReadln(isLn) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_Read; }
private:
    VariableExprAST*      file;
    std::vector<ExprAST*> args;
    bool                  isReadln;
};


class LabelExprAST : public ExprAST
{
public:
    LabelExprAST(const std::vector<int>& lab, ExprAST* st)
	: ExprAST(EK_LabelExpr), labelValues(lab),stmt(st) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen() { assert(0); return 0; }
    llvm::Value* CodeGen(llvm::SwitchInst* inst, llvm::BasicBlock* afterBB, llvm::Type* ty);
    static bool classof(const ExprAST *e) { return e->getKind() == EK_LabelExpr; }
private:
    std::vector<int> labelValues;
    ExprAST*         stmt;
};

class CaseExprAST : public ExprAST
{
public:
    CaseExprAST(ExprAST* e, const std::vector<LabelExprAST*>& lab, ExprAST* other)
	: ExprAST(EK_CaseExpr), expr(e), labels(lab), otherwise(other) {}
    virtual void DoDump(std::ostream& out) const;
    virtual llvm::Value* CodeGen();
    static bool classof(const ExprAST *e) { return e->getKind() == EK_CaseExpr; }
private:
    ExprAST* expr;
    std::vector<LabelExprAST*> labels;
    ExprAST* otherwise;
};

/* Useful global functions */
llvm::Value* MakeIntegerConstant(int val);
llvm::Value* MakeConstant(int val, llvm::Type* ty);
llvm::Value* ErrorV(const std::string& msg);
llvm::Value* FileOrNull(VariableExprAST* file);
bool FileInfo(llvm::Value* f, int& recSize, bool& isText);
bool FileIsText(llvm::Value* f);
#endif
