#include "JSUtilObj.hpp"
#include <v8.h>
#include "../JSObjectScript.hpp"
#include "../JSSerializer.hpp"
#include "JSObjectsUtils.hpp"
#include "JSVec3.hpp"
#include "JSQuaternion.hpp"
#include "../JSUtil.hpp"
#include "../JSObjectStructs/JSUtilStruct.hpp"
#include "../JSObjectStructs/JSVisibleStruct.hpp"
#include "../JSObjectStructs/JSPresenceStruct.hpp"
#include <math.h>

#include <sirikata/core/util/Random.hpp>
#include <sirikata/core/util/Base64.hpp>


namespace Sirikata{
namespace JS{
namespace JSUtilObj{

/**
   Overloads the '-' operator for many types.  a and b must be of the same type
   (either vectors or numbers).  If a and b are vectors (a =
   <ax,ay,az>; b = <bx,by,bz>, returns <ax-bx, ay-by, az-bz>).  If a and b are
   numbers, returns a - b.

   @param a Of type vector or number.
   @param b Of type vector or number.
 */
v8::Handle<v8::Value> ScriptMinus(const v8::Arguments& args)
{
    v8::HandleScope handle_scope;

    if (args.Length() != 2)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: minu requires two arguments.")) );

    String dummyErr;
    //check if numbers
    bool isNum1, isNum2;
    double num1,num2;

    isNum1 = NumericValidate(args[0]);
    if (isNum1)
    {
        isNum2 = NumericValidate(args[1]);
        if (! isNum2)
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: minus requires two arguments of same type.  First argument is number.  Second argument is not.")) );

        num1 = NumericExtract(args[0]);
        num2 = NumericExtract(args[1]);

        return v8::Number::New(num1-num2);
    }

    String errMsg = "Error in JSUtilObj.cpp when trying to subtract.  Could not decode util struct.  ";
    JSUtilStruct* jsutil = JSUtilStruct::decodeUtilStruct(args.This() ,errMsg);
    if (jsutil == NULL)
        return v8::ThrowException( v8::Exception::Error(v8::String::New(errMsg.c_str())) );


    //check if quaternions
    Quaternion q1,q2;
    if (QuaternionValValidate(args[0]))
    {
        if (! QuaternionValValidate(args[1]))
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: minus requires two arguments of same type.  First argument is quat.  Second argument is not.")) );

        q1 = QuaternionValExtract(args[0]);
        q2 = QuaternionValExtract(args[1]);

        q1 = q1 - q2;
        return handle_scope.Close(jsutil->struct_createQuaternion(q1));
    }


    //check if vecs
    Vector3d vec1,vec2;
    if (Vec3ValValidate(args[0]))
    {
        if (! Vec3ValValidate(args[1]))
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: minus requires two arguments of same type.  First argument is vec3.  Second argument is not.")) );


        v8::Handle<v8::Object> o1,o2;
        o1 = args[0]->ToObject();
        o2 = args[1]->ToObject();
        vec1 = Vec3Extract(o1);
        vec2 = Vec3Extract(o2);

        vec1 = vec1-vec2;

        return handle_scope.Close(jsutil->struct_createVec3(vec1));
    }

    return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: minus requires two arguments.  Both must either be vectors or numbers.")) );
}

/**

 */
v8::Handle<v8::Value> ScriptSporef(const v8::Arguments& args)
{
    if (args.Length() == 0)
    {
        SpaceObjectReference sporef = SpaceObjectReference::null();
        return v8::String::New(sporef.toString().c_str(),sporef.toString().size() );
    }

    if (args.Length() > 2)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: spcript sporef requires two or fewer arguments.")));

    //use first arg as space
    INLINE_SPACEID_CONV_ERROR(args[0],ScriptSporef,1,space);

    if (args.Length() == 2)
    {
        INLINE_OBJID_CONV_ERROR(args[1],ScriptSporef,2,obj);
        SpaceObjectReference sporef (space,obj);
        return v8::String::New(sporef.toString().c_str(),sporef.toString().size() );
    }

    //have no object reference, just use space.
    SpaceObjectReference sporef (space,ObjectReference::null());
    return v8::String::New(sporef.toString().c_str(),sporef.toString().size() );
}

v8::Handle<v8::Value> ScriptEqual(const v8::Arguments& args)
{
    if (args.Length() != 2)
        V8_EXCEPTION_CSTR("Error in check equal.  requires only two arguments");

    return v8::Boolean::New(args[0]->Equals(args[1]));
}


v8::Handle<v8::Value> ScriptDiv(const v8::Arguments& args)
{
    if (args.Length() != 2)
        V8_EXCEPTION_CSTR("Error in division.  requires only two arguments");

    INLINE_DOUBLE_CONV_ERROR(args[0],scriptDiv,1,lhs);
    INLINE_DOUBLE_CONV_ERROR(args[1],scriptDiv,2,rhs);

    return v8::Number::New(lhs/rhs);
}

v8::Handle<v8::Value> ScriptMult(const v8::Arguments& args)
{
    if (args.Length() != 2)
        V8_EXCEPTION_CSTR("Error in mult.  requires only two arguments");

    INLINE_DOUBLE_CONV_ERROR(args[0],scriptMult,1,lhs);
    INLINE_DOUBLE_CONV_ERROR(args[1],scriptMult,2,rhs);

    return v8::Number::New(lhs*rhs);
}

v8::Handle<v8::Value> ScriptMod(const v8::Arguments& args)
{
    if (args.Length() != 2)
        V8_EXCEPTION_CSTR("Error in mod.  requires only two arguments");

    INLINE_INTEGER_CONV_ERROR(args[0],scriptMod,1,lhs);
    INLINE_INTEGER_CONV_ERROR(args[1],scriptMod,2,rhs);

    return v8::Number::New(lhs%rhs);
}




/**
   Overloads the '+' operator for many types.  a and b must be of the same type
   (either vectors, numbers, or strings).  If a and b are vectors (a =
   <ax,ay,az>; b = <bx,by,bz>, returns <ax+bx, ay+by, az+bz>).  If a and b are
   numbers, returns a + b.  If a and b are strings, returns concatenated string.

   @param a Of type vector, number, or string.
   @param b Of type vector, number, or string.
 */
v8::Handle<v8::Value> ScriptPlus(const v8::Arguments& args)
{
    if (args.Length() != 2)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments.")) );

    v8::HandleScope handle_scope;
    String dummyErr;
    //check if numbers
    bool isNum1, isNum2;
    double num1,num2;
    isNum1 = NumericValidate(args[0]);
    if (isNum1)
    {
        isNum2 = NumericValidate(args[1]);
        if (! isNum2)
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments of same type.  First argument is number.  Second argument is not.")) );


        num1 = NumericExtract(args[0]);
        num2 = NumericExtract(args[1]);

        return v8::Number::New(num1+num2);
    }

    String errMsg = "Error in JSUtilObj.cpp when trying to subtract.  Could not decode util struct.  ";
    JSUtilStruct* jsutil = JSUtilStruct::decodeUtilStruct(args.This() ,errMsg);
    if (jsutil == NULL)
        return v8::ThrowException( v8::Exception::Error(v8::String::New(errMsg.c_str())) );


    //check if quaternions
    Quaternion q1,q2;
    if (QuaternionValValidate(args[0]))
    {
        if (! QuaternionValValidate(args[1]))
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments of same type.  First argument is quat.  Second argument is not.")) );

        q1 = QuaternionValExtract(args[0]);
        q2 = QuaternionValExtract(args[1]);

        q1 = q1 + q2;
        return handle_scope.Close(jsutil->struct_createQuaternion(q1));
    }



    //check if vecs
    Vector3d vec1,vec2;
    if (Vec3ValValidate(args[0]))
    {
        if (! Vec3ValValidate(args[1]))
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments of same type.  First argument is vec3.  Second argument is not.")) );

        v8::HandleScope handle_scope;
        v8::Handle<v8::Object> o1,o2;
        o1 = args[0]->ToObject();
        o2 = args[1]->ToObject();
        vec1 = Vec3Extract(o1);
        vec2 = Vec3Extract(o2);

        vec1 = vec1+vec2;

        return handle_scope.Close(jsutil->struct_createVec3(vec1));
    }


    //check if strings
    String str1,str2;
    bool isStr1, isStr2;
    isStr1 = decodeString(args[0], str1, dummyErr);
    if (isStr1)
    {
        isStr2 = decodeString(args[1], str2, dummyErr);
        if (!isStr2)
            return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments of same type.  First argument is string.  Second argument is not.")) );

        String newStr = str1+str2;
        return v8::String::New(newStr.c_str(), newStr.size());
    }



    return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: plus requires two arguments.  Both must either be vectors, strings, or numbers.")) );
}



/**
  @return a random float from 0 to 1
 */
v8::Handle<v8::Value> ScriptRandFunction(const v8::Arguments& args)
{
    if (args.Length() != 0)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Invalid parameters passed to rand.")) );

    float rander = randFloat(0,1);
    return v8::Number::New(rander);
}


/**
   @param takes in a single argument
   @return returns a float
*/
v8::Handle<v8::Value> ScriptSqrtFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("sqrt function requires one argument.")) );

    v8::Handle<v8::Value> toSqrt = args[0];

    double d_toSqrt = NumericExtract(toSqrt);

    if (d_toSqrt < 0)
        return v8::ThrowException(v8::Exception::Error(v8::String::New("Invalid parameters passed to sqrt.  Argument must be >=0.")) );

    //return v8::Handle<v8::Number>::New(sqrt(d_toSqrt));
    return v8::Number::New(sqrt(d_toSqrt));
}

/**
   @param float to take arccosine of
   @return angle in radians
 */
v8::Handle<v8::Value> ScriptAcosFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Invalid parameters passed to acos.")) );

    v8::Handle<v8::Value> toSqrt = args[0];

    double d_toSqrt = NumericExtract(toSqrt);

    //return v8::Handle<v8::Number>::New(sqrt(d_toSqrt));
    return v8::Number::New(acos(d_toSqrt));
}

/**
   @param angle in radians to take cosine of
   @return cosine of that angle
 */
v8::Handle<v8::Value> ScriptCosFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Invalid parameters passed to cos.")) );

    v8::Handle<v8::Value> toSqrt = args[0];

    double d_toSqrt = NumericExtract(toSqrt);


    //return v8::Handle<v8::Number>::New(sqrt(d_toSqrt));
    return v8::Number::New(cos(d_toSqrt));
}

/**
   @param angle in radians to take sine of
   @return sine of that angle
 */
v8::Handle<v8::Value> ScriptSinFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Invalid parameters passed to sin.")) );

    v8::Handle<v8::Value> toSqrt = args[0];

    double d_toSqrt = NumericExtract(toSqrt);


    return v8::Number::New(sin(d_toSqrt));
}

/**
   @param float to take arcsine of
   @return angle in radians
 */
v8::Handle<v8::Value> ScriptAsinFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Invalid parameters passed to asin.")) );

    v8::Handle<v8::Value> toSqrt = args[0];

    double d_toSqrt = NumericExtract(toSqrt);


    //return v8::Handle<v8::Number>::New(sqrt(d_toSqrt));
    return v8::Number::New(asin(d_toSqrt));
}

/**
   @param base
   @param exponent
   @return returns base to the exponent
 */
v8::Handle<v8::Value> ScriptPowFunction(const v8::Arguments& args)
{
    if (args.Length() != 2)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: power function requires two arguments.  Expecting <base> and <exponent>")) );

    v8::HandleScope handle_scope;
    double base     = NumericExtract(args[0]);
    double exponent = NumericExtract(args[1]);

    double returner = pow(base,exponent);

    return v8::Number::New( returner );
}

/**
   @param exponent
   @return returns e to the exponent
 */
v8::Handle<v8::Value> ScriptExpFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: exp function requires 1 argument.")) );

    v8::HandleScope handle_scope;
    double exponent = NumericExtract(args[0]);

    double returner = exp(exponent);

    return v8::Number::New( returner );
}


/**
   @param number to take abs of
   @return returns absolute value of argument.
 */
v8::Handle<v8::Value> ScriptAbsFunction(const v8::Arguments& args)
{
    if (args.Length() != 1)
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Error: abs function requires a single argument.  Expecting <number to take absolute value of>")) );

    v8::HandleScope handle_scope;
    double numToAbs     = NumericExtract(args[0]);

    return v8::Number::New( fabs(numToAbs) );

}


v8::Handle<v8::Value> Base64Encode(const v8::Arguments& args) {
    assert(args.Length() == 1);

    v8::HandleScope handle_scope;

    String unencoded;
    String errmsg;
    if (!decodeString(args[0], unencoded, errmsg))
        return v8::ThrowException(v8::Exception::Error(v8::String::New("Got non-string in Base64Encode.")));

    String encoded = Base64::encode(unencoded,true);
    return v8::String::New(encoded.c_str(), encoded.size());
}

v8::Handle<v8::Value> Base64EncodeURL(const v8::Arguments& args) {
    assert(args.Length() == 1);

    v8::HandleScope handle_scope;

    String unencoded;
    String errmsg;
    if (!decodeString(args[0], unencoded, errmsg))
        return v8::ThrowException(v8::Exception::Error(v8::String::New("Got non-string in Base64EncodeURL.")));

    String encoded = Base64::encodeURL(unencoded);
    return v8::String::New(encoded.c_str(), encoded.size());
}

v8::Handle<v8::Value> Base64Decode(const v8::Arguments& args) {
    assert(args.Length() == 1);

    v8::HandleScope handle_scope;

    String encoded;
    String errmsg;
    if (!decodeString(args[0], encoded, errmsg))
        return v8::ThrowException(v8::Exception::Error(v8::String::New("Got non-string in Base64Decode.")));

    String decoded = Base64::decode(encoded);
    return v8::String::New(decoded.c_str(), decoded.size());
}

v8::Handle<v8::Value> Base64DecodeURL(const v8::Arguments& args) {
    assert(args.Length() == 1);

    v8::HandleScope handle_scope;

    String encoded;
    String errmsg;
    if (!decodeString(args[0], encoded, errmsg))
        return v8::ThrowException(v8::Exception::Error(v8::String::New("Got non-string in Base64Decode.")));

    String decoded = Base64::decodeURL(encoded);
    return v8::String::New(decoded.c_str(), decoded.size());
}



}//JSUtilObj namespace
}//JS namespace
}//sirikata namespace
