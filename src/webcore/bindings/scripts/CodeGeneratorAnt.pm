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

# Members whose owning interface the 2008 IDL gets "wrong" for a modern DOM.
#
# The member exists and behaves the same, but Chrome exposes it on another
# prototype, which is observable: Object.getOwnPropertyDescriptor(I.prototype,
# n), `n in obj` on the interface itself, for-in and instanceof-adjacent
# feature detection all see the difference. Probed on 2026-11-10 with
#   python tools/chrome_probe.py --url file:///.../cap_dom.html --js-file <probe>
#   (owners scanned across every DOM prototype, not just the inheritance chain,
#    because a move can go *up* the chain as well as down).
#
# Rule: the generated table follows Chrome, not this IDL. Each row is
#   [ from_iface, name, to_iface, flags, valueType ]
# and the flags/type are the IDL's own (they are not in dispute).
my @kOwnershipMoves = (
    # Chrome: DOM standard puts these on Element (they arrived with the
    # Element namespace/prefix/localName attributes and Element.attributes).
    [ 'Node', 'attributes',   'Element', 0,                   'NamedNodeMap' ],
    [ 'Node', 'namespaceURI', 'Element', 0,                   'DOMString' ],
    [ 'Node', 'prefix',       'Element', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'Node', 'localName',    'Element', 0,                   'DOMString' ],

    # Chrome: box metrics and style live on HTMLElement.prototype (Element is
    # the generic one; client*/scroll* do stay on Element, which the same probe
    # confirmed).
    [ 'Element', 'style',        'HTMLElement', 0, 'CSSStyleDeclaration' ],
    [ 'Element', 'offsetLeft',   'HTMLElement', 0, 'long' ],
    [ 'Element', 'offsetTop',    'HTMLElement', 0, 'long' ],
    [ 'Element', 'offsetWidth',  'HTMLElement', 0, 'long' ],
    [ 'Element', 'offsetHeight', 'HTMLElement', 0, 'long' ],
    [ 'Element', 'offsetParent', 'HTMLElement', 0, 'Element' ],

    # Chrome: id/className/innerHTML/outerHTML/children are Element attributes
    # (children comes from the ParentNode mixin, implemented by Element,
    # Document and DocumentFragment; Element is where a div finds it).
    [ 'HTMLElement', 'id',        'Element', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLElement', 'className', 'Element', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLElement', 'innerHTML', 'Element', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLElement', 'outerHTML', 'Element', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLElement', 'children',  'Element', 0,                    'HTMLCollection' ],

    # Chrome: accessKey/tabIndex are HTMLElement attributes (the 2008 IDL repeats
    # them per element interface: anchor, button, input, select, textarea).
    [ 'HTMLAnchorElement',   'accessKey', 'HTMLElement', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLAnchorElement',   'tabIndex',  'HTMLElement', 'ANTB_ATTR_WRITABLE', 'long' ],
    [ 'HTMLButtonElement',   'accessKey', 'HTMLElement', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLButtonElement',   'tabIndex',  'HTMLElement', 'ANTB_ATTR_WRITABLE', 'long' ],
    [ 'HTMLInputElement',    'accessKey', 'HTMLElement', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLInputElement',    'tabIndex',  'HTMLElement', 'ANTB_ATTR_WRITABLE', 'long' ],
    [ 'HTMLSelectElement',   'tabIndex',  'HTMLElement', 'ANTB_ATTR_WRITABLE', 'long' ],
    [ 'HTMLTextAreaElement', 'accessKey', 'HTMLElement', 'ANTB_ATTR_WRITABLE', 'DOMString' ],
    [ 'HTMLTextAreaElement', 'tabIndex',  'HTMLElement', 'ANTB_ATTR_WRITABLE', 'long' ],
);

sub movedOutOf
{
    my $iface = shift;
    my %out;

    foreach my $move (@kOwnershipMoves) {
        $out{$move->[1]} = 1 if $move->[0] eq $iface;
    }
    return \%out;
}

sub movedInto
{
    my $iface = shift;
    my @in;

    foreach my $move (@kOwnershipMoves) {
        push(@in, $move) if $move->[2] eq $iface;
    }
    return \@in;
}

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
    my $attrCount = 0;   # real entries only: @attrLines also carries comments
    my $movedOut = movedOutOf($name);
    foreach my $attribute (@{$dataNode->attributes}) {
        my $signature = $attribute->signature;
        my $attrName = $signature->name;

        # A member this IDL declares but Chrome owns on another prototype: emit
        # it in the owner's table instead (see @kOwnershipMoves).
        next if $movedOut->{$attrName};

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
        $attrCount++;
    }

    my $movedIn = movedInto($name);
    my %already = map { my $n = $_; $n =~ s/^.*"(\w+)".*$/$1/; ($n => 1) } @attrLines;
    foreach my $move (@{$movedIn}) {
        my $flagText = $move->[3] ? $move->[3] : "0";

        # Several source interfaces can move the same member (accessKey/tabIndex:
        # anchor, button, input, select, textarea all declare them); the target
        # must get the entry once.
        next if $already{$move->[1]};
        $already{$move->[1]} = 1;
        push(@attrLines, "    /* from " . $move->[0] . " (Chrome owner) */");
        push(@attrLines, "    { " . CString($move->[1]) . ", " . $flagText . ", "
                             . CString($move->[4]) . " },");
        $attrCount++;
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

    my $attrArray = $attrCount ? "${symbol}Attributes" : "0";
    my $methodArray = @methodLines ? "${symbol}Methods" : "0";
    my $constantArray = @constantLines ? "${symbol}Constants" : "0";

    $body .= "const AntbInterfaceDef $symbol = {\n";
    $body .= "    " . CString($name) . ",\n";
    $body .= "    " . (@parents ? CString($parent) : "0") . ",\n";
    $body .= "    $attrArray, " . $attrCount . ",\n";
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



