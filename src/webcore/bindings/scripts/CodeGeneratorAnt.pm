#
# Copyright (C) 2026 agave project contributors
#
# The ant backend's IDL code generator (A1 of issues/wip/ant-dom-bindings.md).
#
# Unlike CodeGeneratorJS/QJS - which emit a full bridge implementation per
# interface - this backend emits *data*: one Ant<Interface>.cpp per IDL file
# holding the interface's shape (own attributes with their writability/flags,
# own methods with their arity, constants) plus a file-scope registration call.
# The runtime (AntRuntime.cpp + AntBindings.cpp) stays the only implementation
# of behaviour; IDL decides which names exist and what shape they have.
#
# That split is deliberate: the facade (nodeGetter/nodeSetter and its 340
# strcmp branches) keeps working as the [Custom] layer while interfaces migrate
# one at a time, and a generator bug can only produce a wrong *table*, never a
# wrong attribute value.
#
# Output: <outputdir>/Ant<Interface>.cpp
# Consumers: src/javascript/ant_backend/AntBindings.h (struct ABI)

package CodeGeneratorAnt;

my $outputDir = "";

# Default constructor (called by CodeGenerator.pm).
sub new
{
    my $object = shift;

    $codeGenerator = shift;
    $outputDir = shift;

    bless({ }, $object);
    return $object;
}

sub finish
{
    # Nothing is buffered across interfaces: every GenerateInterface writes its
    # own file, so a failed run leaves the previous generated files alone.
    return 1;
}

# Params: 'idlDocument' struct. Only kept for the file header comment.
sub GenerateModule
{
    my $object = shift;
    my $document = shift;
    my $defines = shift;

    $module = $document->module;
}

# C string literal body: the IDL names/types are identifiers and spaces, but
# escape backslashes/quotes so a future IDL change cannot produce broken C++.
sub CString
{
    my $text = shift;

    return "\"\"" unless defined $text;
    $text =~ s/\\/\\\\/g;
    $text =~ s/"/\\"/g;
    return "\"" . $text . "\"";
}

sub EscapeComment
{
    my $text = shift;

    $text =~ s/\*\//\* \//g;
    return $text;
}

# Params: 'domClass' struct
sub GenerateInterface
{
    my $object = shift;
    my $dataNode = shift;
    my $defines = shift;

    my $name = $dataNode->name;

    # Interfaces that only exist as mixins (SVGTests, SVGLangSpace, ...) are
    # marked ObjCProtocol: they are implemented by other interfaces, never an
    # object of their own, so the DOM has no class/prototype for them.
    return if $dataNode->extendedAttributes->{"ObjCProtocol"};

    # Only the first parent is the prototype-chain parent; the rest are
    # implemented interfaces and their members are already part of this
    # interface's own lists (CodeGenerator's inheritance pass).
    my $parent = "";
    my @parents = @{$dataNode->parents};
    if (@parents) {
        $parent = $codeGenerator->StripModule($parents[0]);
    }

    # ---------------------------------------------------------------- attributes
    my @attrLines = ();
    foreach my $attribute (@{$dataNode->attributes}) {
        my $signature = $attribute->signature;
        my $attrName = $signature->name;

        my @flags = ();
        push(@flags, "ANTB_ATTR_WRITABLE") if $attribute->type !~ /^readonly/;
        push(@flags, "ANTB_ATTR_CUSTOM")
            if $signature->extendedAttributes->{"Custom"}
            || $signature->extendedAttributes->{"CustomGetter"}
            || $signature->extendedAttributes->{"CustomSetter"};
        push(@flags, "ANTB_ATTR_REPLACEABLE") if $signature->extendedAttributes->{"Replaceable"};
        push(@flags, "ANTB_ATTR_PUT_FORWARDS") if $signature->extendedAttributes->{"PutForwards"};

        my $flagText = @flags ? join(" | ", @flags) : "0";
        push(@attrLines, "    { " . CString($attrName) . ", " . $flagText . ", "
                             . CString($signature->type) . " },");
    }

    # ---------------------------------------------------------------- methods
    my @methodLines = ();
    foreach my $function (@{$dataNode->functions}) {
        my $signature = $function->signature;
        my $methodName = $signature->name;

        my @flags = ();
        push(@flags, "ANTB_METHOD_CUSTOM") if $signature->extendedAttributes->{"Custom"};
        my $flagText = @flags ? join(" | ", @flags) : "0";

        # -1 = the IDL does not pin the arity (optional/variadic arguments).
        my $argCount = 0;
        my $optional = 0;
        foreach my $parameter (@{$function->parameters}) {
            if ($parameter->extendedAttributes->{"Optional"}) {
                $optional = 1;
            }
            $argCount++;
        }
        $argCount = -1 if $optional;

        push(@methodLines, "    { " . CString($methodName) . ", " . $argCount . ", "
                                . $flagText . ", " . CString($signature->type) . " },");
    }

    # ---------------------------------------------------------------- constants
    my @constantLines = ();
    foreach my $constant (@{$dataNode->constants}) {
        my $value = $constant->value;
        # IDL constants here are numeric; keep the literal as written so integer
        # constants survive without a float round trip.
        push(@constantLines, "    { " . CString($constant->name) . ", " . $value . " },");
    }

    # ---------------------------------------------------------------- file
    my $symbol = "k" . $name . "Def";
    my $header = <<"EOF";
/* Generated by tools/gen_bindings.ps1 -Generator ANT from the $name IDL. */
/* Do not edit: change the .idl (or CodeGeneratorAnt.pm) and regenerate. */
#include "AntBindings.h"

namespace AntBindings {
namespace Generated {

EOF

    my $body = "";
    if (@attrLines) {
        $body .= "static const AntbAttributeDef ${symbol}Attributes[] = {\n";
        $body .= join("\n", @attrLines) . "\n";
        $body .= "};\n\n";
    }
    if (@methodLines) {
        $body .= "static const AntbMethodDef ${symbol}Methods[] = {\n";
        $body .= join("\n", @methodLines) . "\n";
        $body .= "};\n\n";
    }
    if (@constantLines) {
        $body .= "static const AntbConstantDef ${symbol}Constants[] = {\n";
        $body .= join("\n", @constantLines) . "\n";
        $body .= "};\n\n";
    }

    my $attrArray = @attrLines ? "${symbol}Attributes" : "0";
    my $methodArray = @methodLines ? "${symbol}Methods" : "0";
    my $constantArray = @constantLines ? "${symbol}Constants" : "0";

    $body .= "const AntbInterfaceDef $symbol = {\n";
    $body .= "    " . CString($name) . ",\n";
    $body .= "    " . (@parents ? CString($parent) : "0") . ",\n";
    $body .= "    $attrArray, " . scalar(@attrLines) . ",\n";
    $body .= "    $methodArray, " . scalar(@methodLines) . ",\n";
    $body .= "    $constantArray, " . scalar(@constantLines) . "\n";
    $body .= "};\n\n";

    my $fileName = "$outputDir/Ant$name.cpp";
    open(my $fh, ">", $fileName) || die "Couldn't open file $fileName";

    print $fh $header;
    print $fh $body;
    print $fh "} /* namespace Generated */\n\n";
    print $fh "static const int s_antRegistered" . $name . " = registerInterface(&Generated::$symbol);\n\n";
    print $fh "} /* namespace AntBindings */\n";
    close($fh);
}

1;
