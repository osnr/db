proc Claim {args} { upvar this this; Say [expr {[info exists this] ? $this : "<unknown>"}] claims {*}$args }
proc Wish {args} { upvar this this; Say [expr {[info exists this] ? $this : "<unknown>"}] wishes {*}$args }
proc When {args} {
    set body [lindex $args end]
    set pattern [lreplace $args end end]
    if {[lindex $pattern 0] eq "(non-capturing)"} {
        set argNames [list]; set argValues [list]
        set pattern [lreplace $pattern 0 0]
    } else {
        # lassign [uplevel Evaluator::serializeEnvironment] argNames argValues
        set argNames [list]; set argValues [list]
    }

    set varNamesWillBeBound [list]
    set negate false
    for {set i 0} {$i < [llength $pattern]} {incr i} {
        set word [lindex $pattern $i]
        if {$word eq "&"} {
            # Desugar this join into nested Whens.
            set remainingPattern [lrange $pattern $i+1 end]
            set pattern [lrange $pattern 0 $i-1]
            for {set j 0} {$j < [llength $remainingPattern]} {incr j} {
                set remainingWord [lindex $remainingPattern $j]
                if {[regexp {^/([^/ ]+)/$} $remainingWord -> remainingVarName] &&
                    $remainingVarName in $varNamesWillBeBound} {
                    lset remainingPattern $j \$$remainingVarName
                }
            }
            set body [list When {*}$remainingPattern $body]
            break

        } elseif {[set varName [__scanVariable $word]] != 0} {
            if {[__variableNameIsNonCapturing $varName]} {
            } elseif {$varName eq "nobody" || $varName eq "nothing"} {
                # Rewrite this entire clause to be negated.
                set negate true
            } else {
                # Rewrite subsequent instances of this variable name /x/
                # (in joined clauses) to be bound $x.
                if {[string range $varName 0 2] eq "..."} {
                    set varName [string range $varName 3 end]
                }
                lappend varNamesWillBeBound $varName
            }
        } elseif {[__startsWithDollarSign $word]} {
            lset pattern $i [uplevel [list subst $word]]
        }
    }

    if {$negate} {
        set negateBody [list if {[llength $__matches] == 0} $body]
        Say when the collected matches for $pattern are /__matches/ [list [list {*}$argNames __matches] $negateBody] with environment $argValues
    } else {
        lappend argNames {*}$varNamesWillBeBound
        Say when {*}$pattern [list $argNames $body] with environment $argValues
    }
}