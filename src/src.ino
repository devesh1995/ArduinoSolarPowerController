/* A self-contained algorithm for routing surplus PV power to a dump load.  Uses an 
 * 'energy bucket' to model the operation of a digital supply meter.  Energy measurements 
 * at the supply point are accumulated over each individual mains cycle and added 
 * directly to the bucket.  A safety margin is included in the form of a steady leak in 
 * the bucket.  
 *
 * The energy level is checked every time that the bucket has been updated.  If the 
 * level is above the threshold value, an output pin is set; otherwise it is cleared. 
 * By use of an external trigger device that detects zero-crossings, this signal can be used to 
 * control a triac which will 'fire' or 'clear' at the next zero-crossing point.  The trigger 
 * pin is active low; it operates in parallel with the on-board LED (pin 13) which is 
 * active high.
 * 
 * Although measurements of power are accumulated between +ve going crossings, the triac 
 * operates between consecutive -ve going crossings.  This interleaved behaviour ensures the 
 * fastest possible response time for whole-cycle operation of the triac.
 *
 * The standard high-pass filter is retained only for determining the start of each new
 * cycle of the mains voltage.  DC is removed from the raw V & I samples by 
 * subtracting the known offset as determined by a single low-pass filter.  This LPF is 
 * updated once every mains cycle with the net dc offset of all raw voltage samples taken
 * during the previous cycle.  This approach requires the use of a single (buffered) reference 
 * which is used for both the voltage and current sensors.
 *
 * Each iteration of loop() corresponds to a single pair of V & I measurements, of which 
 * there are several dozen per mains cycle.  This code contains no periodicity greater than 
 * one mains cycle (20mS) and is intended for continuous operation, preferably without use of
 * Serial statements which could disrupt the flow.
 *
 * As it is *energy* which is of prime importance to this application, no independent calibration 
 * of voltage and current measurements has been included.  Having removed the DC-offset, 
 * 'raw' V & I samples are multipled together to give 'raw' power, these instantaneous values 
 * being accumulated during each mains cycle to a measure of'raw' energy.  A single calibration 
 * factor, POWERCAL, is then applied to convert this value into Joules before being added to the 
 * energy bucket.  Its value is not critical because any absolute inaccuracy will cancel
 * out when import and export are constrained to be equal, as this algorithm does. 
 * 
 * A secondary calibration factor, VOLTAGECAL, is used purely for determining the correct point
 * in the voltage waveform when the external trigger device may be safely armed. Each of these
 * calibration factors is described in more detail when their values are assigned.
 *
 *                  Robin Emley (calypso_rae on Open Energy Monitor Forum)
 *                  July 2012
 */


#define POSITIVE 1
#define NEGATIVE 0
#define ON 0  // the external trigger device is active low
#define OFF 1

// for normal operation, the next line should be commented out
#define DEBUG
 
// define the input and output pins
byte outputPinForLed = 13;
byte outputPinForTrigger = 9;
byte voltageSensorPin = 2;
byte currentSensorPin = 1;

float safetyMargin_watts = 0;  // <<<-------  Safety Margin in Watts (increase for more export)
long cycleCount = 0;
int samplesDuringThisMainsCycle = 0;
byte nextStateOfTriac = OFF;
float cyclesPerSecond = 50; // use float to ensure accurate maths

long noOfSamplePairs = 0;
byte polarityNow = NEGATIVE; // probably not important, but better than being indeterminate

boolean triggerNeedsToBeArmed = false;
boolean beyondStartUpPhase = false;

// the'energy bucket' mimics the operation of a digital supply meter at the grid connection point.
float energyInBucket = 0;                                                 
int capacityOfEnergyBucket = 3600; // 0.001 kWh = 3600 Joules

// Local declarations of items to support code that I've lifted from calcVI(), in EmonLib. 
//
#ifdef DEBUG
// use floating point maths to ensure accuracy for synthesised operation
float sampleV;   // raw Voltage sample, 
float lastSampleV;   // saved value from previous loop (HP filter is for voltage samples only)
float sampleI;   //   raw current sample                   
                  
#else
int sampleV,sampleI;   // voltage & current samples are integers in the ADC's input range 0 - 1023 
int lastSampleV;     // stored value from the previous loop (HP filter is for voltage samples only)         
#endif

double lastFilteredV,filteredV;  //  voltage values after filtering to remove the DC offset,
                                 //   and the stored values from the previous loop             
double prevDCoffset;          // <<--- for LPF to quantify the DC offset
double DCoffset;              // <<--- for LPF 
double cumVdeltasThisCycle;   // <<--- for LPF 
double sumP;   //  cumulative sum of power calculations within this mains cycles

float POWERCAL;  // To convert the product of raw V & I samples into Joules.  

float VOLTAGECAL; // To convert raw voltage samples into volts.  Used for determining when
                  // the triggers device can be safely armed

#ifdef DEBUG
// some additional components for use in DEBUG mode
byte noOfVoltageSamplesPerCycle_4debug = 45;
float voltageSamples_4debug[45];
byte vsIndex_4debug = 0;
int powerRatingOfImmersion_4debug = 3000; 
byte triacState_4debug = OFF;
float surplusPV_4debug = 100; // <<<---------------------- PV power in Watts
#endif


void setup()
{  
  Serial.begin(9600);
  pinMode(outputPinForTrigger, OUTPUT);  
  pinMode(outputPinForLed, OUTPUT);  
    
#if defined DEBUG 
  Serial.println("In DEBUG mode ..."); 
  Serial.println(); 

  POWERCAL = 1.0;   // to convert the product of simulated V & I samples into Joules                 
  VOLTAGECAL = 1.0; // to convert raw voltage samples into volts.   
                    // These cal values are unity because the simulation is scaled for volts and amps
                    
  
  /*  populate the voltage sample array for DEBUG use
   */
  float amplitude = 240 * sqrt(2); // in ADC units
  float angleIncrement = (2 * PI) / noOfVoltageSamplesPerCycle_4debug;
  float angleOffset = 0.01; // to avoid sample right at zero crossing point
  float voltage, angle;
  byte index;
  
  for (index = 0; index < noOfVoltageSamplesPerCycle_4debug; index++)
  {
    angle = (index * angleIncrement) + angleOffset;
    voltage = amplitude * sin(angle);
    voltageSamples_4debug[index] = voltage;
  } 
  
#else
  Serial.println("In NORMAL mode ..."); 
  Serial.println(); 

  POWERCAL = 0.085; // Joules per ADC-unit squared.  Used for converting the product of 
                    // voltage and current samples into Joules.
                    //    To determine this value, note the rate that the energy bucket's
                    // level increases when a known load is being measured at a convenient
                    // test location (e.g  using a mains extention with the outer cover removed so that 
                    // the current-clamp can fit around just one core.  Adjust POWERCAL so that
                    // 'measured value' = 'expected value' for various loads.  The value of
                    // POWERCAL is not critical as any absolute error will cancel out when 
                    // import and export flows are balanced.  
                    
  VOLTAGECAL = 1.44;    // Volts per ADC-unit.
                        // This value is used to determine when the voltage level is suitable for 
                        // arming the external trigger device.  To set this value, note the min and max
                        // numbers that are seen when measuring 240Vac via the voltage sensor, which 
                        // is 678.8V p-t-p.  The range on my setup is 471 meaning that I'm under-reading 
                        // voltage by 471/679.  VOLTAGECAL therefore need to be the inverse of this, i.e.
                        // 679/471 or 1.44
                        
#endif
}


void loop() // each loop is for one pair of V & I measurements
{
  noOfSamplePairs++;              // for stats only
  samplesDuringThisMainsCycle++;  // for power calculation at the start of each mains cycle

  lastSampleV=sampleV;            // save previous voltage values for digital high-pass filter
  lastFilteredV = filteredV;      

  //-----------------------------------------------------------------------------
  // A1) Read in the raw voltage sample (this is a synthesised value when in DEBUG mode)
  //-----------------------------------------------------------------------------
#ifdef DEBUG
  sampleV = getNextVoltageSample(); // synthesised value
#else    
  sampleV = analogRead(voltageSensorPin);                 //Read in raw voltage signal


  //-----------------------------------------------------------------------------
  // A2) For normal operation, the raw current sample must be taken immediately after
  //     the voltage sample.  But when running in DEBUG mode, all processing of the 
  //     current sample is deferred.
  //-----------------------------------------------------------------------------
  sampleI = analogRead(currentSensorPin);                 //Read in raw current signal
#endif

  double sampleVminusDC = sampleV - DCoffset;; // This value is needed more than once, so is best 
                                               // determined near the top of the loop. The equivalent
                                               // value for current is deferred until it is needed.  

  //-----------------------------------------------------------------------------
  // B1) Apply digital high pass filter to remove 2.5V DC offset from 
  //     voltage sample in both normal and debug modes.
  //     Further processing of the current sample is deferred.
  //-----------------------------------------------------------------------------
  filteredV = 0.996*(lastFilteredV+sampleV-lastSampleV);
   

  // Establish the polarities of the latest and previous filtered voltage samples
  byte polarityOfLastReading = polarityNow;
  if(filteredV >= 0) 
    polarityNow = POSITIVE; 
  else 
    polarityNow = NEGATIVE;
 
#ifdef DEBUG
  // In normal operation, this algorith will be driving a triac via a trigger device that
  // detects zero-crossings of the voltage waveform.  A triac is capable of changing 
  // state at every zero-crossing.  When in DEBUG, mode, the state of the simulated triac 
  // should similarly be updated at every zero-crossing event.
  //
  if (polarityNow != polarityOfLastReading)
  {
    if(nextStateOfTriac == ON)
      triacState_4debug = true;
    else 
      triacState_4debug = false;
  }
#endif

  if (polarityNow == POSITIVE)
  {
    // --------------------------------------------------------
    // Start of processing that is specific to positive Vsamples
    // --------------------------------------------------------
     
    if (polarityOfLastReading != POSITIVE)
    {
      // This is the start of a new mains cycle
      cycleCount++; // for stats only

      /* update the Low Pass Filter for DC-offset removal
       */
      prevDCoffset = DCoffset;
      DCoffset = prevDCoffset + (0.01 * cumVdeltasThisCycle); 

      //  Calculate the real power of all instantaneous measurements taken during the 
      //  previous mains cycle, and determine the gain (or loss) in energy.
      double realPower = POWERCAL * sumP / (float)samplesDuringThisMainsCycle;
      float realEnergy = realPower / cyclesPerSecond;


      /*--------------------------------
       * WARNING!
       * Before normal operation commences, all Serial statements 
       * should be removed because they are likely to interfere with time-critical code.
       *--------------------------------         
       */
      if((cycleCount % 500) == 5) // display useful data every 10 seconds
      {
        Serial.print("cyc# ");
        Serial.print(cycleCount);
        Serial.print(", sampleV ");
        Serial.print(sampleV);
        Serial.print(", fltdV ");
        Serial.print(filteredV);
        Serial.print(", sampleV-dc ");
        Serial.print((int)(sampleV - DCoffset));
        Serial.print(", cumVdeltas ");
        Serial.print(cumVdeltasThisCycle);
        Serial.print(", prevDCoffset ");
        Serial.print(prevDCoffset);
        Serial.print(", refFltdV ");
        Serial.print(DCoffset);
        Serial.print(", enInBkt ");
        Serial.println(energyInBucket);
//    pause();
  }    
  
/*
      if((cycleCount % 50) == 5) // display bucket energy every second
                                 // useful for calibration trials
      {
        Serial.print("energy in bucket = ");
        Serial.println(energyInBucket); 
      }
*/

      if (beyondStartUpPhase == true)
      {  
        // Providing that the DC-blocking filters have had sufficient time to settle,    
        // add this power contribution to the energy bucket
        energyInBucket += realEnergy;   
         
        // Reduce the level in the energy bucket by the specified safety margin.
        // This allows the system to be positively biassed towards export rather than import
        energyInBucket -= safetyMargin_watts / cyclesPerSecond; 
       

        // Apply max and min limits to bucket's level
        if (energyInBucket > capacityOfEnergyBucket)
          energyInBucket = capacityOfEnergyBucket;  
        if (energyInBucket < 0)
          energyInBucket = 0;  
   
      }
      else
      {  
        // wait until the DC-blocking filters have had time to settle
        if(cycleCount > 100) // 100 mains cycles is 2 seconds
          beyondStartUpPhase = true;
      }
      
      /* Having updated the level in the energy bucket at the startof a new mains cycle, a 
       * decision will soon need to be taken as to the next on/off state of the triac.  
       * Rather than taking this decision now, the algorithm waits until the voltage 
       * condition is known to be suitable for the trigger to be safely armed. A flag is set 
       * now to show that the trigger should be armed at the next opportunity.
       */
      triggerNeedsToBeArmed = true;
      
      /* clear the per-cycle accumulators for use in this new mains cycle.  
       */
      sumP = 0;
      samplesDuringThisMainsCycle = 0;
      cumVdeltasThisCycle = 0;
      
      
      
      
    } // end of processing that is specific to the first +ve Vsample in each new cycle

   
    /* still processing POSITIVE Vsamples ...
     */
    if (triggerNeedsToBeArmed == true)
    {
      // check to see whether the +ve going voltage has reached a suitable level
      // for the trigger to be reliably armed
      if((sampleVminusDC * VOLTAGECAL) > 20)
      {
        // It's now safe to arm the trigger.  So ...
        
        // first check the level in the energy bucket to determine whether the 
        // triac should be fired or not at the next opportunity
        //
        if (energyInBucket > (capacityOfEnergyBucket / 2))        
        {
          nextStateOfTriac = ON;  // the external trigger device is active low
          digitalWrite(outputPinForLed, 1);  // active high
        } 
        else
        {
          nextStateOfTriac = OFF; 
          digitalWrite(outputPinForLed, 0);  
        } 
                  
        // then set the Arduino's output pin accordingly, 
        digitalWrite(outputPinForTrigger, nextStateOfTriac);   
        
        // and clear the flag.
        triggerNeedsToBeArmed = false;
        
#ifdef DEBUG
        // Display relevant values when the trigger device is being armed
        // start to display this data 5 cycles before the bucket gets to half-full 
        // (May not be exactly 5 if the high-pass filters have not fully settled)
        //
//        if (cycleCount > 
//        (100 + (1800/((surplusPV_4debug - safetyMargin_watts)/cyclesPerSecond)) - 5))
/*
        {
          Serial.print("cycle No. ");
          Serial.print(cycleCount);
          Serial.print(", samp'V ");
          Serial.print(sampleV);
          Serial.print(", energyInBucket ");
          Serial.print(energyInBucket);
          Serial.print(", triggerState ");
          Serial.println(nextStateOfTriac);
          delay(100);
          //pause();     
        }
*/
#endif

      }
    }    
  }  // end of processing that is specific to positive Vsamples

  { 
    //  No specific processing is required for negative Vsamples
  }
  
  
  // Processing for ALL Vsamples, both positive and negative
  //------------------------------------------------------------
  
#ifdef DEBUG
// The current sample in DEBUG mode can now be synthesized.  Because the system is linear,
// the value of the current sample can be based on that of the voltage sample.
// First remove the known amount of DC-offset that is used in debug mode.
   float voltage = sampleV - 500;
   
   // Next, calculate the current for the known values of surplus-PV and instantaneous voltage.
   // For 1kW, the rms current at 240V is (1000W / 240V) Amps.  For different values of 
   // voltage and power, this value should be adjusted accordingly
   //
   float current_PV = (1000.0/240.0) * (voltage/240.0) * (surplusPV_4debug/1000.0);
   //      Amps     =   Watts/Volts  *   Volts/Volts   *   Watts/Watts
   
   // Next, calculate the current that is taken by the immersion heater, but only if the
   // triac is 'on'.  This current is in the opposite direction, hence the minus sign.
   //
   float current_immersion = 0;
   if (triacState_4debug == 1){
     current_immersion = -1 * (1000.0/240.0) * (voltage/240.0) * (powerRatingOfImmersion_4debug/1000.0);}
   //      Amps        = minus  Watts/Volts  *   Volts/Volts   *   Watts/Watts
  
   // Now combine these two currents and add DC offset as the Arduino's ADC would do
   //
   sampleI = (current_PV + current_immersion) + 500; 
#endif 

//double  sampleVminusDC = sampleV - DCoffset;  // this line has moved to higher up the loop
  double  sampleIminusDC = sampleI - DCoffset;
  
  double instP = sampleVminusDC * sampleIminusDC; //  power contribution for this pair of V&I samples 
  sumP +=instP;     // cumulative power values for this mains cycle 

  cumVdeltasThisCycle += (sampleV - DCoffset); // for use with LP filter

/*
        // can watch power contributions accumulating here, prior to being added to the energy bucket
//        if ((beyondStartUpPhase == true) 
        if (((cycleCount % 50) == 1) && (samplesDuringThisMainsCycle == 42)) // i.e. the 42nd sample of 
                                                                             // every 50th cycle
        {
          Serial.print("cyc ");
          Serial.print(cycleCount);
          Serial.print(", sam'V ");
          Serial.print(sampleV);
          Serial.print(", sam'I ");
          Serial.print(sampleI);
          Serial.print(", DCoffset ");
          Serial.print(DCoffset);
          Serial.print(", instP ");
          Serial.print(instP);
          Serial.print(", sumP ");
          Serial.print(sumP);
          Serial.print(", bkt'En ");
          Serial.println(energyInBucket);
          //delay(400);
          //pause();     
        }
*/       

} // end of loop()


#ifdef DEBUG
// function to synthesive voltage samples for DEBUG mode only
float getNextVoltageSample()
{
  float voltageSample;

  voltageSample = voltageSamples_4debug[vsIndex_4debug];
  voltageSample+= 500.0; // not critical, approx mid-way in the ADC's input range.
  
  vsIndex_4debug++;  
  if (vsIndex_4debug >= noOfVoltageSamplesPerCycle_4debug) 
  {
    vsIndex_4debug = 0;
  }

  return voltageSample;
}
#endif

/* A function which causes the code to pause until any key, followed by [C/R], is pressed.  
 */
#ifdef DEBUG
void pause()
{
  byte done = false;
  byte dummyByte;
   
  while (done != true)
  {
    if (Serial.available() > 0)
    {
      dummyByte = Serial.read(); // to 'consume' the incoming byte
      done++;
    }
  }    
}
#endif
