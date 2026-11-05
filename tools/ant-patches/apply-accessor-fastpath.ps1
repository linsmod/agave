# tools/ant-patches/apply-accessor-fastpath.ps1
#
# Accessor (getter/setter) calls are the hottest path in a Vue-style app: the
# micro benchmarks in tools/bench show
#
#   plain property read     ~7 ns
#   accessor get           ~45 ns   (before this patch ~130 ns)
#   accessor set          ~190 ns   (before this patch ~160 ns, still open)
#
# Part 1 (applied): skip sv_vm_call's call-plan construction for plain
# synchronous closures.  ant.c call_proto_accessor() now tries
# sv_callback_prepare()/sv_callback_call() first, which invokes the closure
# directly, and only falls back to sv_vm_call() for exotic cases.
#
# Part 2 (applied, may not hit yet): js_call_setter() plus a branch in
# silver/ops/property.h sv_put_field_try_cached() that calls an own-property
# setter directly instead of returning false and going through the proxy-aware
# setprop_impl() fallback.
#
# Usage:  pwsh -File tools\ant-patches\apply-accessor-fastpath.ps1 [-AntRoot H:\ant-v16] [-Build]
param(
    [string]$AntRoot = 'H:\ant-v16',
    [switch]$Build
)

$ErrorActionPreference = 'Stop'

$antC = Join-Path $AntRoot 'src\ant.c'
$internalH = Join-Path $AntRoot 'include\internal.h'
$propertyH = Join-Path $AntRoot 'src\silver\ops\property.h'

foreach ($f in @($antC, $internalH, $propertyH)) {
    if (-not (Test-Path $f)) { throw "not found: $f" }
}

# 1. declaration
$text = [IO.File]::ReadAllText($internalH)
if ($text -notmatch 'js_call_setter') {
    $anchor = 'ant_value_t js_call_getter(ant_t *js, ant_value_t receiver, ant_value_t getter);'
    $text = $text.Replace($anchor, $anchor + "`r`nant_value_t js_call_setter(ant_t *js, ant_value_t receiver, ant_value_t setter, ant_value_t value);")
    [IO.File]::WriteAllText($internalH, $text)
    Write-Host 'internal.h: js_call_setter declared'
}

# 2. fast getter/setter call + helper
$text = [IO.File]::ReadAllText($antC)
if ($text -notmatch 'sv_callback_prepare\(js, accessor, prim\)') {
    $old = @'
  ant_value_t result = sv_vm_call(js->vm, js, accessor, prim, arg, arg_count, NULL, js_mkundef());
'@.Replace("`r`n", "`n").TrimEnd()
    $new = @'
  /* Fast path: a plain synchronous closure (Object.defineProperty getters and
   * class accessors are the common case) is called directly, skipping the
   * call-plan construction sv_vm_call does for every call. */
  {
    sv_callback_t cb = sv_callback_prepare(js, accessor, prim);

    if (cb.closure) {
      ant_value_t fast = sv_callback_call(js->vm, js, &cb, arg, arg_count);

      if (is_setter)
        return is_err(fast) ? fast : (arg ? *arg : js_mkundef());

      return fast;
    }
  }

  ant_value_t result = sv_vm_call(js->vm, js, accessor, prim, arg, arg_count, NULL, js_mkundef());
'@.Replace("`r`n", "`n").TrimEnd()
    $normalized = $text.Replace("`r`n", "`n")
    if (-not $normalized.Contains($old)) { throw 'call_proto_accessor tail not found' }
    $text = $text.Replace($old.Replace("`n", "`r`n"), $new.Replace("`n", "`r`n"))
    Write-Host 'ant.c: accessor fast path added'
}

if ($text -notmatch 'js_call_setter\(ant_t \*js') {
    $anchor = @'
ant_value_t js_call_getter(ant_t *js, ant_value_t receiver, ant_value_t getter) {
  if (vtype(getter) != kTypeBuiltin) return call_proto_accessor(js, receiver, getter, true, NULL, 0, false);
  return sv_call_builtin_this(js, getter, receiver, NULL, 0, js_mkundef());
}
'@.Replace("`r`n", "`n").TrimEnd()
    $addition = $anchor + @'


ant_value_t js_call_setter(ant_t *js, ant_value_t receiver, ant_value_t setter, ant_value_t value) {
  return call_proto_accessor(js, receiver, setter, true, &value, 1, true);
}
'@.Replace("`r`n", "`n").TrimEnd()
    $normalized = $text.Replace("`r`n", "`n")
    if (-not $normalized.Contains($anchor)) { throw 'js_call_getter body not found' }
    $text = $text.Replace($anchor.Replace("`n", "`r`n"), $addition.Replace("`n", "`r`n"))
    Write-Host 'ant.c: js_call_setter added'
}

[IO.File]::WriteAllText($antC, $text)

# 3. own-property setter branch in the VM put path
$text = [IO.File]::ReadAllText($propertyH)
if ($text -notmatch 'js_call_setter\(js, obj, prop->setter, val\)') {
    $old = @'
        prop->key.interned == a->str &&
        !prop->has_getter &&
        !prop->has_setter &&
        (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0) {
      ant_object_prop_set_unchecked(ptr, ic->cached_index, val);
      gc_write_barrier_prop(js, ptr, val);
      ant_property_mutation_invalidate(js, ptr, a->str);
      return true;
    }
'@.Replace("`r`n", "`n").TrimEnd()
    $new = @'
        prop->key.interned == a->str) {
      /* Accessor with a setter: call it here instead of falling back to the
       * proxy-aware setprop_impl() for every assignment. */
      if (prop->has_setter && is_callable(prop->setter)) {
        *out = js_call_setter(js, obj, prop->setter, val);
        return true;
      }

      if (!prop->has_getter && !prop->has_setter &&
          (prop->attrs & ANT_PROP_ATTR_WRITABLE) != 0) {
        ant_object_prop_set_unchecked(ptr, ic->cached_index, val);
        gc_write_barrier_prop(js, ptr, val);
        ant_property_mutation_invalidate(js, ptr, a->str);
        return true;
      }
    }
'@.Replace("`r`n", "`n").TrimEnd()
    $normalized = $text.Replace("`r`n", "`n")
    if (-not $normalized.Contains($old)) { throw 'put_field block not found' }
    $text = $text.Replace($old.Replace("`n", "`r`n"), $new.Replace("`n", "`r`n"))
    [IO.File]::WriteAllText($propertyH, $text)
    Write-Host 'property.h: setter fast path added'
}

if ($Build) {
    $env:PATH = "C:\msys64\usr\bin;C:\msys64\mingw64\bin;" + $env:PATH
    & 'C:\msys64\mingw64\bin\ninja.exe' -C (Join-Path $AntRoot 'build-zig') libant.a
    if ($LASTEXITCODE -ne 0) { throw "libant build failed ($LASTEXITCODE)" }
    Write-Host 'libant.a rebuilt - now run tools\build_antjsb.ps1 -Force'
}
