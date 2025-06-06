/*********************                                                        */
/*! \file ReluConstraint.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Guy Katz, Parth Shah, Derek Huang
 ** This file is part of the Marabou project.
 ** Copyright (c) 2017-2024 by the authors listed in the file AUTHORS
 ** in the top-level source directory) and their institutional affiliations.
 ** All rights reserved. See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** See the description of the class in ReluConstraint.h.
 **/

#include "ReluConstraint.h"

#include "Debug.h"
#include "DivideStrategy.h"
#include "FloatUtils.h"
#include "GlobalConfiguration.h"
#include "ITableau.h"
#include "InfeasibleQueryException.h"
#include "MStringf.h"
#include "MarabouError.h"
#include "PiecewiseLinearCaseSplit.h"
#include "PiecewiseLinearConstraint.h"
#include "Query.h"
#include "Statistics.h"
#include "TableauRow.h"

#ifdef _WIN32
#define __attribute__( x )
#endif

ReluConstraint::ReluConstraint( unsigned b, unsigned f )
    : PiecewiseLinearConstraint( TWO_PHASE_PIECEWISE_LINEAR_CONSTRAINT )
    , _b( b )
    , _f( f )
    , _auxVarInUse( false )
    , _direction( PHASE_NOT_FIXED )
    , _haveEliminatedVariables( false )
    , _tighteningRow( NULL )
{
}

ReluConstraint::ReluConstraint( const String &serializedRelu )
    : _haveEliminatedVariables( false )
    , _tighteningRow( NULL )
{
    String constraintType = serializedRelu.substring( 0, 4 );
    ASSERT( constraintType == String( "relu" ) );

    // Remove the constraint type in serialized form
    String serializedValues = serializedRelu.substring( 5, serializedRelu.length() - 5 );
    List<String> values = serializedValues.tokenize( "," );

    ASSERT( values.size() >= 2 && values.size() <= 3 );

    if ( values.size() == 2 )
    {
        auto var = values.begin();
        _f = atoi( var->ascii() );
        ++var;
        _b = atoi( var->ascii() );

        _auxVarInUse = false;
    }
    else
    {
        auto var = values.begin();
        _f = atoi( var->ascii() );
        ++var;
        _b = atoi( var->ascii() );
        ++var;
        _aux = atoi( var->ascii() );

        _auxVarInUse = true;
    }
}

PiecewiseLinearFunctionType ReluConstraint::getType() const
{
    return PiecewiseLinearFunctionType::RELU;
}

PiecewiseLinearConstraint *ReluConstraint::duplicateConstraint() const
{
    ReluConstraint *clone = new ReluConstraint( _b, _f );
    *clone = *this;
    this->initializeDuplicateCDOs( clone );
    return clone;
}

void ReluConstraint::restoreState( const PiecewiseLinearConstraint *state )
{
    const ReluConstraint *relu = dynamic_cast<const ReluConstraint *>( state );

    CVC4::context::CDO<bool> *activeStatus = _cdConstraintActive;
    CVC4::context::CDO<PhaseStatus> *phaseStatus = _cdPhaseStatus;
    CVC4::context::CDList<PhaseStatus> *infeasibleCases = _cdInfeasibleCases;
    *this = *relu;
    _cdConstraintActive = activeStatus;
    _cdPhaseStatus = phaseStatus;
    _cdInfeasibleCases = infeasibleCases;
}

void ReluConstraint::registerAsWatcher( ITableau *tableau )
{
    tableau->registerToWatchVariable( this, _b );
    tableau->registerToWatchVariable( this, _f );

    if ( _auxVarInUse )
        tableau->registerToWatchVariable( this, _aux );
}

void ReluConstraint::unregisterAsWatcher( ITableau *tableau )
{
    tableau->unregisterToWatchVariable( this, _b );
    tableau->unregisterToWatchVariable( this, _f );

    if ( _auxVarInUse )
        tableau->unregisterToWatchVariable( this, _aux );
}

void ReluConstraint::checkIfLowerBoundUpdateFixesPhase( unsigned variable, double bound )
{
    if ( variable == _f && FloatUtils::isPositive( bound ) )
        setPhaseStatus( RELU_PHASE_ACTIVE );
    else if ( variable == _b && !FloatUtils::isNegative( bound ) )
        setPhaseStatus( RELU_PHASE_ACTIVE );
    else if ( _auxVarInUse && variable == _aux && FloatUtils::isPositive( bound ) )
        setPhaseStatus( RELU_PHASE_INACTIVE );
}

void ReluConstraint::checkIfUpperBoundUpdateFixesPhase( unsigned variable, double bound )
{
    if ( ( variable == _f || variable == _b ) && !FloatUtils::isPositive( bound ) )
        setPhaseStatus( RELU_PHASE_INACTIVE );

    if ( _auxVarInUse && variable == _aux && FloatUtils::isZero( bound ) )
        setPhaseStatus( RELU_PHASE_ACTIVE );
}

void ReluConstraint::notifyLowerBound( unsigned variable, double newBound )
{
    if ( _statistics )
        _statistics->incLongAttribute( Statistics::NUM_BOUND_NOTIFICATIONS_TO_PL_CONSTRAINTS );

    if ( _boundManager == nullptr )
    {
        if ( existsLowerBound( variable ) &&
             !FloatUtils::gt( newBound, getLowerBound( variable ) ) )
            return;
        setLowerBound( variable, newBound );
        checkIfLowerBoundUpdateFixesPhase( variable, newBound );
    }
    else if ( !phaseFixed() )
    {
        ASSERT( _boundManager != nullptr );

        double bound = getLowerBound( variable );
        checkIfLowerBoundUpdateFixesPhase( variable, bound );
        if ( isActive() )
        {
            bool proofs = _boundManager->shouldProduceProofs();

            if ( proofs )
                createTighteningRow();

            // A positive lower bound is always propagated between f and b
            if ( ( variable == _f || variable == _b ) && bound > 0 )
            {
                // If we're in the active phase, aux should be 0
                if ( proofs && _auxVarInUse )
                    _boundManager->addLemmaExplanationAndTightenBound(
                        _aux, 0, Tightening::UB, { variable }, Tightening::LB, getType() );
                else if ( !proofs && _auxVarInUse )
                    _boundManager->tightenUpperBound( _aux, 0 );

                // After updating to active phase
                unsigned partner = ( variable == _f ) ? _b : _f;
                _boundManager->tightenLowerBound( partner, bound, *_tighteningRow );
            }

            // If b is non-negative, we're in the active phase
            else if ( _auxVarInUse && variable == _b && FloatUtils::isZero( bound ) )
            {
                if ( proofs && _auxVarInUse )
                    _boundManager->addLemmaExplanationAndTightenBound(
                        _aux, 0, Tightening::UB, { variable }, Tightening::LB, getType() );
                else if ( !proofs && _auxVarInUse )
                    _boundManager->tightenUpperBound( _aux, 0 );
            }

            // A positive lower bound for aux means we're inactive: f is 0, b is
            // non-positive When inactive, b = -aux
            else if ( _auxVarInUse && variable == _aux && bound > 0 )
            {
                if ( proofs )
                    _boundManager->addLemmaExplanationAndTightenBound(
                        _f, 0, Tightening::UB, { variable }, Tightening::LB, getType() );
                else
                    _boundManager->tightenUpperBound( _f, 0 );

                // After updating to inactive phase
                _boundManager->tightenUpperBound( _b, -bound, *_tighteningRow );
            }

            // A negative lower bound for b could tighten aux's upper bound
            else if ( _auxVarInUse && variable == _b && bound < 0 )
            {
                if ( proofs )
                {
                    // If already inactive, tightening is linear
                    if ( _phaseStatus == RELU_PHASE_INACTIVE )
                        _boundManager->tightenUpperBound( _aux, -bound, *_tighteningRow );
                    else if ( _phaseStatus == PHASE_NOT_FIXED )
                        _boundManager->addLemmaExplanationAndTightenBound(
                            _aux, -bound, Tightening::UB, { variable }, Tightening::LB, getType() );
                }
                else
                    _boundManager->tightenUpperBound( _aux, -bound );
            }

            // Also, if for some reason we only know a negative lower bound for
            // f, we attempt to tighten it to 0
            else if ( bound < 0 && variable == _f )
            {
                if ( proofs )
                    _boundManager->addLemmaExplanationAndTightenBound(
                        _f, 0, Tightening::LB, { variable }, Tightening::LB, getType() );
                else
                    _boundManager->tightenLowerBound( _f, 0 );
            }
        }
    }
}

void ReluConstraint::notifyUpperBound( unsigned variable, double newBound )
{
    if ( _statistics )
        _statistics->incLongAttribute( Statistics::NUM_BOUND_NOTIFICATIONS_TO_PL_CONSTRAINTS );

    if ( _boundManager == nullptr )
    {
        if ( existsUpperBound( variable ) &&
             !FloatUtils::lt( newBound, getUpperBound( variable ) ) )
            return;

        setUpperBound( variable, newBound );
        checkIfUpperBoundUpdateFixesPhase( variable, newBound );
    }
    else if ( !phaseFixed() )
    {
        ASSERT( _boundManager != nullptr );
        double bound = getUpperBound( variable );
        checkIfUpperBoundUpdateFixesPhase( variable, bound );

        if ( isActive() )
        {
            bool proofs = _boundManager->shouldProduceProofs();

            if ( proofs )
                createTighteningRow();

            if ( variable == _f )
            {
                if ( proofs )
                {
                    if ( _phaseStatus != RELU_PHASE_INACTIVE )
                        _boundManager->tightenUpperBound( _b, bound, *_tighteningRow );
                    else
                    {
                        if ( FloatUtils::isZero( bound ) )
                            _boundManager->addLemmaExplanationAndTightenBound(
                                _b, 0, Tightening::UB, { variable }, Tightening::UB, getType() );
                        // Bound cannot be negative if ReLU is inactive
                        else if ( FloatUtils::isNegative( bound ) )
                            throw InfeasibleQueryException();
                    }
                }
                else
                    _boundManager->tightenUpperBound( _b, bound );
            }
            else if ( variable == _b )
            {
                if ( !FloatUtils::isPositive( bound ) )
                {
                    // If b has a non-positive upper bound, f's upper bound is 0
                    if ( proofs )
                        _boundManager->addLemmaExplanationAndTightenBound(
                            _f, 0, Tightening::UB, { variable }, Tightening::UB, getType() );
                    else
                        _boundManager->tightenUpperBound( _f, 0 );

                    // Aux's range is minus the range of b
                    // After updating to inactive phase
                    if ( _auxVarInUse )
                        _boundManager->tightenLowerBound( _aux, -bound, *_tighteningRow );
                }
                else
                {
                    // b has a positive upper bound, propagate to f
                    if ( proofs )
                    {
                        // If already inactive, tightening is linear
                        if ( _phaseStatus == RELU_PHASE_ACTIVE )
                            _boundManager->tightenUpperBound( _f, bound, *_tighteningRow );
                        else if ( _phaseStatus == PHASE_NOT_FIXED )
                            _boundManager->addLemmaExplanationAndTightenBound( _f,
                                                                               bound,
                                                                               Tightening::UB,
                                                                               { variable },
                                                                               Tightening::UB,
                                                                               getType() );
                    }
                    else
                        _boundManager->tightenUpperBound( _f, bound );
                }
            }
            else if ( _auxVarInUse && variable == _aux )
            {
                if ( proofs )
                {
                    if ( _phaseStatus != RELU_PHASE_ACTIVE )
                        _boundManager->tightenLowerBound( _b, -bound, *_tighteningRow );
                    else
                    {
                        if ( FloatUtils::isZero( bound ) )
                            _boundManager->addLemmaExplanationAndTightenBound(
                                _b, 0, Tightening::LB, { variable }, Tightening::UB, getType() );
                        // Bound cannot be negative if ReLU is active
                        else if ( FloatUtils::isNegative( bound ) )
                            throw InfeasibleQueryException();
                    }
                }
                else
                    _boundManager->tightenLowerBound( _b, -bound );
            }
        }
    }
}

bool ReluConstraint::participatingVariable( unsigned variable ) const
{
    return ( variable == _b ) || ( variable == _f ) || ( _auxVarInUse && variable == _aux );
}

List<unsigned> ReluConstraint::getParticipatingVariables() const
{
    return _auxVarInUse ? List<unsigned>( { _b, _f, _aux } ) : List<unsigned>( { _b, _f } );
}

bool ReluConstraint::satisfied() const
{
    if ( !( existsAssignment( _b ) && existsAssignment( _f ) ) )
        throw MarabouError( MarabouError::PARTICIPATING_VARIABLE_MISSING_ASSIGNMENT );

    double bValue = getAssignment( _b );
    double fValue = getAssignment( _f );

    if ( FloatUtils::isNegative( fValue ) )
        return false;

    if ( FloatUtils::isPositive( fValue ) )
        return FloatUtils::areEqual(
            bValue, fValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE );
    else
        return !FloatUtils::isPositive( bValue );
}

List<PiecewiseLinearConstraint::Fix> ReluConstraint::getPossibleFixes() const
{
    // Reluplex does not currently work with Gurobi.
    ASSERT( _gurobi == NULL );

    ASSERT( !satisfied() );
    ASSERT( existsAssignment( _b ) );
    ASSERT( existsAssignment( _f ) );

    double bValue = getAssignment( _b );
    double fValue = getAssignment( _f );

    ASSERT(
        !FloatUtils::isNegative( fValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE ) );

    List<PiecewiseLinearConstraint::Fix> fixes;

    // Possible violations:
    //   1. f is positive, b is positive, b and f are disequal
    //   2. f is positive, b is non-positive
    //   3. f is zero, b is positive
    if ( FloatUtils::isPositive( fValue ) )
    {
        if ( FloatUtils::isPositive( bValue ) )
        {
            fixes.append( PiecewiseLinearConstraint::Fix( _b, fValue ) );
            fixes.append( PiecewiseLinearConstraint::Fix( _f, bValue ) );
        }
        else
        {
            if ( _direction == RELU_PHASE_INACTIVE )
            {
                fixes.append( PiecewiseLinearConstraint::Fix( _f, 0 ) );
                fixes.append( PiecewiseLinearConstraint::Fix( _b, fValue ) );
            }
            else
            {
                fixes.append( PiecewiseLinearConstraint::Fix( _b, fValue ) );
                fixes.append( PiecewiseLinearConstraint::Fix( _f, 0 ) );
            }
        }
    }
    else
    {
        if ( _direction == RELU_PHASE_ACTIVE )
        {
            fixes.append( PiecewiseLinearConstraint::Fix( _f, bValue ) );
            fixes.append( PiecewiseLinearConstraint::Fix( _b, 0 ) );
        }
        else
        {
            fixes.append( PiecewiseLinearConstraint::Fix( _b, 0 ) );
            fixes.append( PiecewiseLinearConstraint::Fix( _f, bValue ) );
        }
    }

    return fixes;
}

List<PiecewiseLinearConstraint::Fix> ReluConstraint::getSmartFixes( ITableau *tableau ) const
{
    // Reluplex does not currently work with Gurobi.
    ASSERT( _gurobi == NULL );

    ASSERT( !satisfied() );
    ASSERT( existsAssignment( _f ) && existsAssignment( _b ) );

    double bDeltaToFDelta;
    double fDeltaToBDelta;
    bool linearlyDependent =
        tableau->areLinearlyDependent( _b, _f, bDeltaToFDelta, fDeltaToBDelta );

    /*
      If b and f are linearly independent, there's nothing clever to be done -
      just return the "non-smart" fixes.

      We could potentially do something if both are basic, but for now we
      return the non-smart fixes. Some dependency may be created when f or b are
      pivoted out of the base; in which case we hope getSmartFixes will be called
      again later, where we will be able to produce smart fixes.
    */
    if ( !linearlyDependent )
        return getPossibleFixes();

    bool fIsBasic = tableau->isBasic( _f );
    bool bIsBasic = tableau->isBasic( _b );
    ASSERT( bIsBasic != fIsBasic );

    List<PiecewiseLinearConstraint::Fix> fixes;
    /*
      We know b and f are linearly dependent. This means that one of them
      is basic, the other non basic, and that coefficient is not 0.

      We know that:

        _f = ... + coefficient * _b + ...

      Next, we want to compute by how much we need to change b and/or f in order to
      repair the violation. For example, if we have:

        b = 0, f = 6

      and

        b = ... -2f ...

      And we want to repair so that f=b, we do the following computation:

        f' = f - x
        b' = b +2x
        f' = b'
        -------->
        0 + 2x = 6 - x
        -------->
        x = 2

      Giving us that we need to decrease f by 2, which will cause b to be increased
      by 4, repairing the violation. Of course, there may be multiple options for repair.
    */

    double bValue = getAssignment( _b );
    double fValue = getAssignment( _f );

    /*
      Repair option number 1: the active fix. We want to set f = b > 0.
    */

    if ( !bIsBasic )
    {
        /*
          bValue + delta = fValue + bDeltaToFDelta * delta
          delta = ( bValue - fValue ) / ( bDeltaToFDelta - 1 );
        */

        if ( !FloatUtils::areEqual( bDeltaToFDelta, 1.0 ) )
        {
            double activeFixDelta = ( bValue - fValue ) / ( bDeltaToFDelta - 1 );
            double activeFix = bValue + activeFixDelta;
            fixes.append( PiecewiseLinearConstraint::Fix( _b, activeFix ) );
        }
    }
    else
    {
        /*
          fValue + delta = bValue + fDeltaToBDelta * delta
          delta = ( fValue - bValue ) / ( fDeltaToBDelta - 1 );
        */
        if ( !FloatUtils::areEqual( fDeltaToBDelta, 1.0 ) )
        {
            double activeFixDelta = ( fValue - bValue ) / ( fDeltaToBDelta - 1 );
            double activeFix = fValue + activeFixDelta;
            fixes.append( PiecewiseLinearConstraint::Fix( _f, activeFix ) );
        }
    }

    /*
      Repair option number 2: the inactive fix. We want to set f = 0, b < 0.
    */

    if ( !fIsBasic )
    {
        double newBValue = bValue + fDeltaToBDelta * ( -fValue );
        if ( newBValue <= 0 )
            fixes.append( PiecewiseLinearConstraint::Fix( _f, 0 ) );
    }
    else
    {
        /*
          By how much should we change b to make f zero?

          fValue + bDeltaToFDelta * delta = 0
          delta = fValue / ( -bDeltaToFDelta )
        */
        double nonactiveFixDelta = fValue / ( -bDeltaToFDelta );
        double nonactiveFix = bValue + nonactiveFixDelta;

        if ( nonactiveFix <= 0 )
            fixes.append( PiecewiseLinearConstraint::Fix( _b, nonactiveFix ) );
    }

    return fixes;
}

List<PiecewiseLinearCaseSplit> ReluConstraint::getCaseSplits() const
{
    if ( _phaseStatus != PHASE_NOT_FIXED )
        throw MarabouError( MarabouError::REQUESTED_CASE_SPLITS_FROM_FIXED_CONSTRAINT );

    List<PiecewiseLinearCaseSplit> splits;

    if ( _direction == RELU_PHASE_INACTIVE )
    {
        splits.append( getInactiveSplit() );
        splits.append( getActiveSplit() );
        return splits;
    }
    if ( _direction == RELU_PHASE_ACTIVE )
    {
        splits.append( getActiveSplit() );
        splits.append( getInactiveSplit() );
        return splits;
    }

    // If we have existing knowledge about the assignment, use it to
    // influence the order of splits
    if ( existsAssignment( _f ) )
    {
        if ( FloatUtils::isPositive( getAssignment( _f ) ) )
        {
            splits.append( getActiveSplit() );
            splits.append( getInactiveSplit() );
        }
        else
        {
            splits.append( getInactiveSplit() );
            splits.append( getActiveSplit() );
        }
    }
    else
    {
        // Default: start with the inactive case, because it doesn't
        // introduce a new equation and is hence computationally cheaper.
        splits.append( getInactiveSplit() );
        splits.append( getActiveSplit() );
    }

    return splits;
}

List<PhaseStatus> ReluConstraint::getAllCases() const
{
    if ( _direction == RELU_PHASE_INACTIVE )
        return { RELU_PHASE_INACTIVE, RELU_PHASE_ACTIVE };

    if ( _direction == RELU_PHASE_ACTIVE )
        return { RELU_PHASE_ACTIVE, RELU_PHASE_INACTIVE };

    // If we have existing knowledge about the assignment, use it to
    // influence the order of splits
    if ( existsAssignment( _f ) )
    {
        if ( FloatUtils::isPositive( getAssignment( _f ) ) )
            return { RELU_PHASE_ACTIVE, RELU_PHASE_INACTIVE };
        else
            return { RELU_PHASE_INACTIVE, RELU_PHASE_ACTIVE };
    }
    else
        return { RELU_PHASE_INACTIVE, RELU_PHASE_ACTIVE };
}

PiecewiseLinearCaseSplit ReluConstraint::getCaseSplit( PhaseStatus phase ) const
{
    if ( phase == RELU_PHASE_INACTIVE )
        return getInactiveSplit();
    else if ( phase == RELU_PHASE_ACTIVE )
        return getActiveSplit();
    else
        throw MarabouError( MarabouError::REQUESTED_NONEXISTENT_CASE_SPLIT );
}

PiecewiseLinearCaseSplit ReluConstraint::getInactiveSplit() const
{
    // Inactive phase: b <= 0, f = 0
    PiecewiseLinearCaseSplit inactivePhase;
    inactivePhase.storeBoundTightening( Tightening( _b, 0.0, Tightening::UB ) );
    inactivePhase.storeBoundTightening( Tightening( _f, 0.0, Tightening::UB ) );
    return inactivePhase;
}

PiecewiseLinearCaseSplit ReluConstraint::getActiveSplit() const
{
    // Active phase: b >= 0, b - f = 0
    PiecewiseLinearCaseSplit activePhase;
    activePhase.storeBoundTightening( Tightening( _b, 0.0, Tightening::LB ) );

    if ( _auxVarInUse )
    {
        // Special case: aux var in use.
        // Because aux = f - b and aux >= 0, we just add that aux <= 0.
        activePhase.storeBoundTightening( Tightening( _aux, 0.0, Tightening::UB ) );
    }
    else
    {
        Equation activeEquation( Equation::EQ );
        activeEquation.addAddend( 1, _b );
        activeEquation.addAddend( -1, _f );
        activeEquation.setScalar( 0 );
        activePhase.addEquation( activeEquation );
    }

    return activePhase;
}

bool ReluConstraint::phaseFixed() const
{
    return _phaseStatus != PHASE_NOT_FIXED;
}

PiecewiseLinearCaseSplit ReluConstraint::getImpliedCaseSplit() const
{
    ASSERT( _phaseStatus != PHASE_NOT_FIXED );

    if ( _phaseStatus == RELU_PHASE_ACTIVE )
        return getActiveSplit();

    return getInactiveSplit();
}

PiecewiseLinearCaseSplit ReluConstraint::getValidCaseSplit() const
{
    return getImpliedCaseSplit();
}

void ReluConstraint::dump( String &output ) const
{
    output = Stringf( "ReluConstraint: x%u = ReLU( x%u ). Active? %s. PhaseStatus = %u (%s).\n",
                      _f,
                      _b,
                      _constraintActive ? "Yes" : "No",
                      _phaseStatus,
                      phaseToString( _phaseStatus ).ascii() );

    output +=
        Stringf( "b in [%s, %s], ",
                 existsLowerBound( _b ) ? Stringf( "%lf", getLowerBound( _b ) ).ascii() : "-inf",
                 existsUpperBound( _b ) ? Stringf( "%lf", getUpperBound( _b ) ).ascii() : "inf" );

    output +=
        Stringf( "f in [%s, %s]",
                 existsLowerBound( _f ) ? Stringf( "%lf", getLowerBound( _f ) ).ascii() : "-inf",
                 existsUpperBound( _f ) ? Stringf( "%lf", getUpperBound( _f ) ).ascii() : "inf" );

    if ( _auxVarInUse )
    {
        output += Stringf(
            ". Aux var: %u. Range: [%s, %s]\n",
            _aux,
            existsLowerBound( _aux ) ? Stringf( "%lf", getLowerBound( _aux ) ).ascii() : "-inf",
            existsUpperBound( _aux ) ? Stringf( "%lf", getUpperBound( _aux ) ).ascii() : "inf" );
    }
}

void ReluConstraint::updateVariableIndex( unsigned oldIndex, unsigned newIndex )
{
    // Variable reindexing can only occur in preprocessing before Gurobi is
    // registered.
    ASSERT( _gurobi == NULL );

    ASSERT( oldIndex == _b || oldIndex == _f || ( _auxVarInUse && oldIndex == _aux ) );
    ASSERT( !_lowerBounds.exists( newIndex ) && !_upperBounds.exists( newIndex ) &&
            newIndex != _b && newIndex != _f && ( !_auxVarInUse || newIndex != _aux ) );

    if ( _lowerBounds.exists( oldIndex ) )
    {
        _lowerBounds[newIndex] = _lowerBounds.get( oldIndex );
        _lowerBounds.erase( oldIndex );
    }

    if ( _upperBounds.exists( oldIndex ) )
    {
        _upperBounds[newIndex] = _upperBounds.get( oldIndex );
        _upperBounds.erase( oldIndex );
    }

    if ( oldIndex == _b )
        _b = newIndex;
    else if ( oldIndex == _f )
        _f = newIndex;
    else
        _aux = newIndex;
}

void ReluConstraint::eliminateVariable( __attribute__( ( unused ) ) unsigned variable,
                                        __attribute__( ( unused ) ) double fixedValue )
{
    ASSERT( variable == _b || variable == _f || ( _auxVarInUse && variable == _aux ) );

    DEBUG( {
        if ( variable == _f )
        {
            ASSERT( FloatUtils::gte( fixedValue, 0.0 ) );
        }

        if ( variable == _f || variable == _b )
        {
            if ( FloatUtils::gt( fixedValue, 0 ) )
            {
                ASSERT( _phaseStatus != RELU_PHASE_INACTIVE );
            }
            else if ( FloatUtils::lt( fixedValue, 0 ) )
            {
                ASSERT( _phaseStatus != RELU_PHASE_ACTIVE );
            }
        }
        else
        {
            // This is the aux variable
            if ( FloatUtils::isPositive( fixedValue ) )
            {
                ASSERT( _phaseStatus != RELU_PHASE_ACTIVE );
            }
        }
    } );

    // In a ReLU constraint, if a variable is removed the entire constraint can be discarded.
    _haveEliminatedVariables = true;
}

bool ReluConstraint::constraintObsolete() const
{
    return _haveEliminatedVariables;
}

void ReluConstraint::getEntailedTightenings( List<Tightening> &tightenings ) const
{
    ASSERT( existsLowerBound( _b ) && existsLowerBound( _f ) && existsUpperBound( _b ) &&
            existsUpperBound( _f ) );

    ASSERT( !_auxVarInUse || ( existsLowerBound( _aux ) && existsUpperBound( _aux ) ) );

    double bLowerBound = getLowerBound( _b );
    double fLowerBound = getLowerBound( _f );

    double bUpperBound = getUpperBound( _b );
    double fUpperBound = getUpperBound( _f );

    double auxLowerBound = 0;
    double auxUpperBound = 0;

    if ( _auxVarInUse )
    {
        auxLowerBound = getLowerBound( _aux );
        auxUpperBound = getUpperBound( _aux );
    }

    // Determine if we are in the active phase, inactive phase or unknown phase
    if ( !FloatUtils::isNegative( bLowerBound ) || FloatUtils::isPositive( fLowerBound ) ||
         ( _auxVarInUse && FloatUtils::isZero( auxUpperBound ) ) )
    {
        // Active case;

        // All bounds are propagated between b and f
        tightenings.append( Tightening( _b, fLowerBound, Tightening::LB ) );
        tightenings.append( Tightening( _f, bLowerBound, Tightening::LB ) );

        tightenings.append( Tightening( _b, fUpperBound, Tightening::UB ) );
        tightenings.append( Tightening( _f, bUpperBound, Tightening::UB ) );

        // Aux is zero
        if ( _auxVarInUse )
        {
            tightenings.append( Tightening( _aux, 0, Tightening::LB ) );
            tightenings.append( Tightening( _aux, 0, Tightening::UB ) );
        }

        tightenings.append( Tightening( _b, 0, Tightening::LB ) );
        tightenings.append( Tightening( _f, 0, Tightening::LB ) );
    }
    else if ( FloatUtils::isNegative( bUpperBound ) || FloatUtils::isZero( fUpperBound ) ||
              ( _auxVarInUse && FloatUtils::isPositive( auxLowerBound ) ) )
    {
        // Inactive case

        // f is zero
        tightenings.append( Tightening( _f, 0, Tightening::LB ) );
        tightenings.append( Tightening( _f, 0, Tightening::UB ) );

        // b is non-positive
        tightenings.append( Tightening( _b, 0, Tightening::UB ) );

        // aux = -b, aux is non-negative
        if ( _auxVarInUse )
        {
            tightenings.append( Tightening( _aux, -bLowerBound, Tightening::UB ) );
            tightenings.append( Tightening( _aux, -bUpperBound, Tightening::LB ) );

            tightenings.append( Tightening( _b, -auxLowerBound, Tightening::UB ) );
            tightenings.append( Tightening( _b, -auxUpperBound, Tightening::LB ) );

            tightenings.append( Tightening( _aux, 0, Tightening::LB ) );
        }
    }
    else
    {
        // Unknown case

        // b and f share upper bounds
        tightenings.append( Tightening( _b, fUpperBound, Tightening::UB ) );
        tightenings.append( Tightening( _f, bUpperBound, Tightening::UB ) );

        // aux upper bound is -b lower bound
        if ( _auxVarInUse )
        {
            tightenings.append( Tightening( _b, -auxUpperBound, Tightening::LB ) );
            tightenings.append( Tightening( _aux, -bLowerBound, Tightening::UB ) );
        }

        // f and aux are always non negative
        tightenings.append( Tightening( _f, 0, Tightening::LB ) );
        if ( _auxVarInUse )
            tightenings.append( Tightening( _aux, 0, Tightening::LB ) );
    }
}

String ReluConstraint::phaseToString( PhaseStatus phase )
{
    switch ( phase )
    {
    case PHASE_NOT_FIXED:
        return "PHASE_NOT_FIXED";

    case RELU_PHASE_ACTIVE:
        return "RELU_PHASE_ACTIVE";

    case RELU_PHASE_INACTIVE:
        return "RELU_PHASE_INACTIVE";

    default:
        return "UNKNOWN";
    }
};

void ReluConstraint::transformToUseAuxVariables( Query &inputQuery )
{
    /*
      We want to add the equation

          f >= b

      Which actually becomes

          f - b - aux = 0

      Lower bound: always non-negative
      Upper bound: when f = 0 and b is minimal, i.e. -b.lb
    */
    if ( _auxVarInUse )
        return;

    // Create the aux variable
    _aux = inputQuery.getNumberOfVariables();
    inputQuery.setNumberOfVariables( _aux + 1 );

    // Create and add the equation
    Equation equation( Equation::EQ );
    equation.addAddend( 1.0, _f );
    equation.addAddend( -1.0, _b );
    equation.addAddend( -1.0, _aux );
    equation.setScalar( 0 );
    inputQuery.addEquation( equation );

    // Adjust the bounds for the new variable
    inputQuery.setLowerBound( _aux, 0 );

    double bLowerBounds =
        existsLowerBound( _b ) ? getLowerBound( _b ) : FloatUtils::negativeInfinity();

    // Generally, aux.ub = -b.lb. However, if b.lb is positive (active
    // phase), then aux.ub needs to be 0
    double auxUpperBound = bLowerBounds > 0 ? 0 : -bLowerBounds;
    inputQuery.setUpperBound( _aux, auxUpperBound );

    // We now care about the auxiliary variable, as well
    _auxVarInUse = true;
}

void ReluConstraint::getCostFunctionComponent( LinearExpression &cost, PhaseStatus phase ) const
{
    // If the constraint is not active or is fixed, it contributes nothing
    if ( !isActive() || phaseFixed() )
        return;

    // This should not be called when the linear constraints have
    // not been satisfied
    ASSERT( !haveOutOfBoundVariables() );

    ASSERT( phase == RELU_PHASE_ACTIVE || phase == RELU_PHASE_INACTIVE );

    if ( phase == RELU_PHASE_INACTIVE )
    {
        // The cost term corresponding to the inactive phase is just f,
        // since the ReLU is inactive and satisfied iff f is 0 and minimal.
        if ( !cost._addends.exists( _f ) )
            cost._addends[_f] = 0;
        cost._addends[_f] = cost._addends[_f] + 1;
    }
    else
    {
        // The cost term corresponding to the inactive phase is f - b,
        // since the ReLU is active and satisfied iff f - b is 0 and minimal.
        // Note that this is true only when we added the constraint that f >= b.
        if ( !cost._addends.exists( _f ) )
            cost._addends[_f] = 0;
        if ( !cost._addends.exists( _b ) )
            cost._addends[_b] = 0;
        cost._addends[_f] = cost._addends[_f] + 1;
        cost._addends[_b] = cost._addends[_b] - 1;
    }
}

PhaseStatus
ReluConstraint::getPhaseStatusInAssignment( const Map<unsigned, double> &assignment ) const
{
    ASSERT( assignment.exists( _b ) );
    return FloatUtils::isNegative( assignment[_b] ) ? RELU_PHASE_INACTIVE : RELU_PHASE_ACTIVE;
}

bool ReluConstraint::haveOutOfBoundVariables() const
{
    double bValue = getAssignment( _b );
    double fValue = getAssignment( _f );

    if ( FloatUtils::gt(
             getLowerBound( _b ), bValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE ) ||
         FloatUtils::lt(
             getUpperBound( _b ), bValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE ) )
        return true;

    if ( FloatUtils::gt(
             getLowerBound( _f ), fValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE ) ||
         FloatUtils::lt(
             getUpperBound( _f ), fValue, GlobalConfiguration::CONSTRAINT_COMPARISON_TOLERANCE ) )
        return true;

    return false;
}

String ReluConstraint::serializeToString() const
{
    // Output format is: relu,f,b,aux
    if ( _auxVarInUse )
        return Stringf( "relu,%u,%u,%u", _f, _b, _aux );

    return Stringf( "relu,%u,%u", _f, _b );
}

unsigned ReluConstraint::getB() const
{
    return _b;
}

unsigned ReluConstraint::getF() const
{
    return _f;
}

bool ReluConstraint::supportPolarity() const
{
    return true;
}

bool ReluConstraint::supportBaBsr() const
{
    return true;
}

bool ReluConstraint::auxVariableInUse() const
{
    return _auxVarInUse;
}

unsigned ReluConstraint::getAux() const
{
    return _aux;
}


double ReluConstraint::computeBaBsr() const
{
    if ( !_networkLevelReasoner )
        throw MarabouError( MarabouError::NETWORK_LEVEL_REASONER_NOT_AVAILABLE );

    double biasTerm = _networkLevelReasoner->getPreviousBias( this );

    // get upper and lower bounds
    double ub = getUpperBound( _b );
    double lb = getLowerBound( _b );

    // cast _b and _f to doubles
    double reluInput = _tableau->getValue( _b );  // ReLU input before activation
    double reluOutput = _tableau->getValue( _f ); // ReLU output after activation

    // compute ReLU score
    double scaler = ub / ( ub - lb );
    double term1 =
        std::min( scaler * reluInput * biasTerm, ( scaler - 1.0 ) * reluInput * biasTerm );
    double term2 = ( scaler * lb ) * reluOutput;

    return term1 - term2;
}

double ReluConstraint::computePolarity() const
{
    double currentLb = getLowerBound( _b );
    double currentUb = getUpperBound( _b );
    if ( currentLb >= 0 )
        return 1;
    if ( currentUb <= 0 )
        return -1;
    double width = currentUb - currentLb;
    double sum = currentUb + currentLb;
    return sum / width;
}

void ReluConstraint::updateDirection()
{
    _direction = ( computePolarity() > 0 ) ? RELU_PHASE_ACTIVE : RELU_PHASE_INACTIVE;
}

PhaseStatus ReluConstraint::getDirection() const
{
    return _direction;
}

void ReluConstraint::updateScoreBasedOnBaBsr()
{
    _score = std::abs( computeBaBsr() );
}

void ReluConstraint::updateScoreBasedOnPolarity()
{
    _score = std::abs( computePolarity() );
}

void ReluConstraint::createTighteningRow()
{
    // Create the row only when needed and when not already created
    if ( !_boundManager->getBoundExplainer() || _tighteningRow || !_auxVarInUse ||
         _tableauAuxVars.empty() )
        return;

    _tighteningRow = std::unique_ptr<TableauRow>( new TableauRow( 3 ) );

    // f = b + aux + counterpart (an additional aux variable of tableau)
    _tighteningRow->_lhs = _f;
    _tighteningRow->_row[0] = TableauRow::Entry( _b, 1 );
    _tighteningRow->_row[1] = TableauRow::Entry( _aux, 1 );
    _tighteningRow->_row[2] = TableauRow::Entry( _tableauAuxVars.back(), 1 );
    _tighteningRow->_scalar = 0;
}

const List<unsigned> ReluConstraint::getNativeAuxVars() const
{
    if ( _auxVarInUse )
        return { _aux };
    return {};
}

void ReluConstraint::addTableauAuxVar( unsigned tableauAuxVar, unsigned constraintAuxVar )
{
    ASSERT( _tableauAuxVars.empty() );

    if ( constraintAuxVar == _aux )
        _tableauAuxVars.append( tableauAuxVar );
}

//
// Local Variables:
// compile-command: "make -C ../.. "
// tags-file-name: "../../TAGS"
// c-basic-offset: 4
// End:
//
