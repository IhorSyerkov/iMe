// ATxmega32C4 microcontroller http://www.atmel.com/Images/Atmel-8493-8-and-32-bit-AVR-XMEGA-Microcontrollers-ATxmega16C4-ATxmega32C4_Datasheet.pdf
// 5V 4A 2.1mmx5.5mm DC power supply
// USB type B connection


// Header files
extern "C" {
	#include <asf.h>
}
#include <string.h>
#include "common.h"
#include "eeprom.h"
#include "fan.h"
#include "gcode.h"
#include "heater.h"
#include "led.h"
#include "motors.h"


// Definitions
#define REQUEST_BUFFER_SIZE 10
#define WAIT_TIMER MOTORS_VREF_TIMER
#define WAIT_TIMER_PERIOD MOTORS_VREF_TIMER_PERIOD

// Unknown pin (Connected to transistors above the microcontroller. Maybe related to detecting if USB is connected)
#define UNKNOWN_PIN IOPORT_CREATE_PIN(PORTA, 1)

// Unused pins (None of them are connected to anything, so they could be used to easily connect additional hardware to the printer)
#define UNUSED_PIN_1 IOPORT_CREATE_PIN(PORTA, 6)
#define UNUSED_PIN_2 IOPORT_CREATE_PIN(PORTB, 0)
#define UNUSED_PIN_3 IOPORT_CREATE_PIN(PORTE, 0)
#define UNUSED_PIN_4 IOPORT_CREATE_PIN(PORTR, 0)
#define UNUSED_PIN_5 IOPORT_CREATE_PIN(PORTR, 1)


// Global variables
char serialNumber[EEPROM_SERIAL_NUMBER_LENGTH];
Gcode requests[REQUEST_BUFFER_SIZE];
uint16_t waitTimerCounter;
bool emergencyStopOccured = false;
Fan fan;
Heater heater;
Led led;
Motors motors;


// Function prototypes

/*
Name: CDC RX notify callback
Purpose: Callback for when USB receives data
*/
void cdcRxNotifyCallback(uint8_t port);

/*
Name: CDC disconnect callback
Purpose: Callback for when USB is disconnected from host
*/
void cdcDisconnectCallback(uint8_t port);

/*
Name: Disable sending wait responses
Purpose: Disables sending wait responses every second
*/
void disableSendingWaitResponses();

/*
Name: Enable sending wait responses
Purpose: Enabled sending wait responses every second
*/
void enableSendingWaitResponses();


// Main function
int main() {
	
	// Initialize system clock
	sysclk_init();
	
	// Initialize interrupt controller
	pmic_init();
	pmic_set_scheduling(PMIC_SCH_ROUND_ROBIN);
	
	// Initialize board
	board_init();
	
	// Initialize I/O ports
	ioport_init();
	
	// Initialize requests
	for(uint8_t i = 0; i < REQUEST_BUFFER_SIZE; i++)
		requests[i].commandParameters = 0;
	
	// Initialize variables
	uint64_t currentCommandNumber = 0;
	uint8_t currentProcessingRequest = 0;
	char responseBuffer[UINT8_MAX + 1];
	char numberBuffer[INT_BUFFER_SIZE];
	
	// Configure ADC Vref pin
	ioport_set_pin_dir(ADC_VREF_PIN, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(ADC_VREF_PIN, IOPORT_MODE_PULLDOWN);
	
	// Enable ADC module
	adc_enable(&ADC_MODULE);
	
	// Initialize peripherals
	fan.initialize();
	heater.initialize();
	led.initialize();
	motors.initialize();
	
	// Configure unknown pin
	ioport_set_pin_dir(UNKNOWN_PIN, IOPORT_DIR_OUTPUT);
	ioport_set_pin_level(UNKNOWN_PIN, IOPORT_PIN_LEVEL_LOW);
	
	// Configure unused pins
	ioport_set_pin_dir(UNUSED_PIN_1, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(UNUSED_PIN_1, IOPORT_MODE_PULLUP);
	ioport_set_pin_dir(UNUSED_PIN_2, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(UNUSED_PIN_2, IOPORT_MODE_PULLUP);
	ioport_set_pin_dir(UNUSED_PIN_3, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(UNUSED_PIN_3, IOPORT_MODE_PULLUP);
	ioport_set_pin_dir(UNUSED_PIN_4, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(UNUSED_PIN_4, IOPORT_MODE_PULLUP);
	ioport_set_pin_dir(UNUSED_PIN_5, IOPORT_DIR_INPUT);
	ioport_set_pin_mode(UNUSED_PIN_5, IOPORT_MODE_PULLUP);
	
	// Configure send wait interrupt
	tc_set_overflow_interrupt_callback(&WAIT_TIMER, []() -> void {
	
		// Check if one second has passed
		if(++waitTimerCounter >= sysclk_get_cpu_hz() / WAIT_TIMER_PERIOD) {
		
			// Reset wait timer counter
			waitTimerCounter = 0;
			
			// Send wait
			sendDataToUsb("wait\n", true);
		}
	});
	
	// Fix writing to EEPROM addresses above 0x2E0 by first writing to an address less than that
	nvm_eeprom_write_byte(0, nvm_eeprom_read_byte(0));
	
	// Read serial number from EEPROM
	nvm_eeprom_write_byte(EEPROM_SERIAL_NUMBER_OFFSET + EEPROM_SERIAL_NUMBER_LENGTH - 1, 0);
	nvm_eeprom_read_buffer(EEPROM_SERIAL_NUMBER_OFFSET, serialNumber, EEPROM_SERIAL_NUMBER_LENGTH);
	
	// Enable interrupts
	cpu_irq_enable();
	
	// Initialize USB
	udc_start();
	
	// Enable sending wait responses
	enableSendingWaitResponses();
	
	// Main loop
	while(true) {
	
		// Check if a current processing request is ready
		if(requests[currentProcessingRequest].commandParameters) {
		
			// Disable sending wait responses
			disableSendingWaitResponses();
			
			// Check if an emergency stop hasn't occured
			if(!emergencyStopOccured) {
		
				// Check if accelerometer isn't working
				if(!motors.accelerometer.isWorking)
				
					// Set response to error
					strcpy(responseBuffer, "Error: Accelerometer isn't working");
				
				// Check if heater isn't working
				else if(!heater.isWorking)
				
					// Set response to error
					strcpy(responseBuffer, "Error: Heater isn't working");
				
				// Otherwise
				else {
				
					// Clear response buffer
					*responseBuffer = 0;
	
					// Check if command contains valid G-code
					if(requests[currentProcessingRequest].commandParameters & ~(VALID_CHECKSUM_OFFSET | PARSED_OFFSET)) {

						// Check if command has host command
						if(requests[currentProcessingRequest].commandParameters & PARAMETER_HOST_COMMAND_OFFSET)
		
							// Set response to error
							strcpy(responseBuffer, "Error: Unknown host command");
	
						// Otherwise
						else {

							// Check if command has an N parameter
							if(requests[currentProcessingRequest].commandParameters & PARAMETER_N_OFFSET) {
			
								// Check if command doesn't have a valid checksum
								if(!(requests[currentProcessingRequest].commandParameters & VALID_CHECKSUM_OFFSET))
	
									// Set response to resend
									strcpy(responseBuffer, "rs");
				
								// Otherwise
								else {
								
									// Check if command is a starting command number
									if(requests[currentProcessingRequest].valueM == 110)
	
										// Set current command number
										currentCommandNumber = requests[currentProcessingRequest].valueN;
									
									// Otherwise check if current command number is at its max
									else if(currentCommandNumber == UINT64_MAX)
	
										// Set response to error
										strcpy(responseBuffer, "Error: Max command number exceeded");
		
									// Otherwise check if command has already been processed
									else if(requests[currentProcessingRequest].valueN < currentCommandNumber)
		
										// Set response to skip
										strcpy(responseBuffer, "skip");
	
									// Otherwise check if an older command was expected
									else if(requests[currentProcessingRequest].valueN > currentCommandNumber)
	
										// Set response to resend
										strcpy(responseBuffer, "rs");
								}
							}
	
							// Check if response wasn't set
							if(!*responseBuffer) {
			
								// Check if command has an M parameter
								if(requests[currentProcessingRequest].commandParameters & PARAMETER_M_OFFSET) {
	
									switch(requests[currentProcessingRequest].valueM) {
										
										// M17
										case 17:
						
											// Turn on motors
											motors.turnOn();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
					
										// M18
										case 18:
						
											// Turn off motors
											motors.turnOff();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// M104 or M109
										case 104:
										case 109:
										
											{
												// Check if temperature is valid
												int32_t temperature = requests[currentProcessingRequest].commandParameters & PARAMETER_S_OFFSET ? requests[currentProcessingRequest].valueS : 0;
												if(!temperature || (temperature >= HEATER_MIN_TEMPERATURE && temperature <= HEATER_MAX_TEMPERATURE)) {
							
													// Set temperature
													heater.setTemperature(temperature, temperature && requests[currentProcessingRequest].valueM == 109);
						
													// Set response to confirmation
													strcpy(responseBuffer, "ok");
												}
							
												// Otherwise
												else
							
													// Set response to temperature range
													strcpy(responseBuffer, "Error: Temperature must be between " TOSTRING(HEATER_MIN_TEMPERATURE) " and " TOSTRING(HEATER_MAX_TEMPERATURE) " degrees Celsius");
											}
										break;
						
										// M105
										case 105:
			
											// Set response to temperature
											strcpy(responseBuffer, "ok\nT:");
											ftoa(heater.getTemperature(), numberBuffer);
											strcat(responseBuffer, numberBuffer);
										break;
						
										// M106 or M107
										case 106:
										case 107:
						
											// Set fan's speed
											fan.setSpeed(requests[currentProcessingRequest].valueM == 106 && requests[currentProcessingRequest].commandParameters & PARAMETER_S_OFFSET ? requests[currentProcessingRequest].valueS : FAN_MIN_SPEED);
						
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// M114
										case 114:
						
											// Set response to confirmation and motors current X
											strcpy(responseBuffer, "ok\nX:");
											ftoa(motors.currentValues[X], numberBuffer);
											strcat(responseBuffer, numberBuffer);
						
											// Append motors current Y to response
											strcat(responseBuffer, " Y:");
											ftoa(motors.currentValues[Y], numberBuffer);
											strcat(responseBuffer, numberBuffer);
							
											// Append motors current Z to response
											strcat(responseBuffer, " Z:");
											ftoa(motors.currentValues[Z], numberBuffer);
											strcat(responseBuffer, numberBuffer);
						
											// Append motors current E to response
											strcat(responseBuffer, " E:");
											ftoa(motors.currentValues[E], numberBuffer);
											strcat(responseBuffer, numberBuffer);
										break;
				
										// M115
										case 115:
				
											// Check if command is to reset
											if(requests[currentProcessingRequest].valueS == 628)
				
												// Reset
												reset_do_soft_reset();
				
											// Otherwise
											else {
				
												// Set response to device and firmware details
												strcpy(responseBuffer, "ok\nPROTOCOL:RepRap FIRMWARE_NAME:" TOSTRING(FIRMWARE_NAME) " FIRMWARE_VERSION:" TOSTRING(FIRMWARE_VERSION) " MACHINE_TYPE:Micro_3D SERIAL_NUMBER:");
												strcat(responseBuffer, serialNumber);
											}
										break;
						
										// M117
										case 117:
						
											// Set response to valid values
											strcpy(responseBuffer, "ok\nXV:");
											strcat(responseBuffer, motors.currentStateOfValues[X] ? "1" : "0");
											strcat(responseBuffer, " YV:");
											strcat(responseBuffer, motors.currentStateOfValues[Y] ? "1" : "0");
											strcat(responseBuffer, " ZV:");
											strcat(responseBuffer, motors.currentStateOfValues[Z] ? "1" : "0");
										break;
										
										// M404
										case 404:
										
											// Set response to reset cause
											strcpy(responseBuffer, "ok\nRC:");
											ulltoa(reset_cause_get_causes(), numberBuffer);
											strcat(responseBuffer, numberBuffer);
										break;
						
										// M420
										case 420:
										
											// Set LED's brightness
											led.setBrightness(requests[currentProcessingRequest].commandParameters & PARAMETER_T_OFFSET ? requests[currentProcessingRequest].valueT : LED_MAX_BRIGHTNESS);
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
										
										// M583
										case 583:
										
											// Set response to if gantry clips are detected
											strcpy(responseBuffer, "ok\nC");
											strcat(responseBuffer, motors.gantryClipsDetected() ? "1" : "0");
										break;
					
										// M618 or M619
										case 618:
										case 619:
					
											// Check if EEPROM offset and length are provided
											if(requests[currentProcessingRequest].commandParameters & (PARAMETER_S_OFFSET | PARAMETER_T_OFFSET)) {
						
												// Check if offset and length are valid
												int32_t offset = requests[currentProcessingRequest].valueS;
												uint8_t length = requests[currentProcessingRequest].valueT;
							
												if(offset >= 0 && length && length <= sizeof(UINT32_MAX) && offset + length < EEPROM_SIZE) {
								
													// Set response to offset
													strcpy(responseBuffer, "ok\nPT:");
													ulltoa(offset, numberBuffer);
													strcat(responseBuffer, numberBuffer);
									
													// Check if reading an EEPROM value
													if(requests[currentProcessingRequest].valueM == 619) {
									
														// Get value from EEPROM
														uint32_t value = 0;
														nvm_eeprom_read_buffer(offset, &value, length);
								
														// Append value to response
														strcat(responseBuffer, " DT:");
														ulltoa(value, numberBuffer);
														strcat(responseBuffer, numberBuffer);
													}
									
													// Otherwise check if EEPROM value is provided
													else if(requests[currentProcessingRequest].commandParameters & PARAMETER_P_OFFSET) {
							
														// Write value to EEPROM
														nvm_eeprom_erase_and_write_buffer(offset, &requests[currentProcessingRequest].valueP, length);
														
														// Check if value changes the bed height
														if(offset == EEPROM_BED_ORIENTATION_BACK_RIGHT_OFFSET || offset == EEPROM_BED_ORIENTATION_BACK_LEFT_OFFSET || offset == EEPROM_BED_ORIENTATION_FRONT_LEFT_OFFSET || offset == EEPROM_BED_ORIENTATION_FRONT_RIGHT_OFFSET || offset == EEPROM_BED_OFFSET_BACK_LEFT_OFFSET || offset == EEPROM_BED_OFFSET_BACK_RIGHT_OFFSET || offset == EEPROM_BED_OFFSET_FRONT_RIGHT_OFFSET || offset == EEPROM_BED_OFFSET_FRONT_LEFT_OFFSET || offset == EEPROM_BED_HEIGHT_OFFSET_OFFSET)
															
															// Update bed changes
															motors.updateBedChanges();
													}
									
													// Otherwise
													else
									
														// Clear response buffer
														*responseBuffer = 0;
												}
											}
										break;
										
										// M5321
										case 5321:
										
											// Check if hours is provided
											if(requests[currentProcessingRequest].commandParameters & PARAMETER_X_OFFSET) {
											
												// Update hours counter in EEPROM
												float hoursCounter;
												nvm_eeprom_read_buffer(EEPROM_HOURS_COUNTER_OFFSET, &hoursCounter, EEPROM_HOURS_COUNTER_LENGTH);
												hoursCounter += requests[currentProcessingRequest].valueX;
												nvm_eeprom_erase_and_write_buffer(EEPROM_HOURS_COUNTER_OFFSET, &hoursCounter, EEPROM_HOURS_COUNTER_LENGTH);
												
												// Set response to confirmation
												strcpy(responseBuffer, "ok");
											}
										break;
						
										// M20, M21, M80, M82, M83, M84, M110, M111, or M999
										case 20:
										case 21:
										case 80:
										case 82:
										case 83:
										case 84:
										case 110:
										case 111:
										case 999:
				
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
									}
								}
			
								// Otherwise check if command has a G parameter
								else if(requests[currentProcessingRequest].commandParameters & PARAMETER_G_OFFSET) {
	
									switch(requests[currentProcessingRequest].valueG) {
		
										// G0 or G1
										case 0:
										case 1:
									
											// Check if command doesn't contain an E value or the heater is on
											if(!(requests[currentProcessingRequest].commandParameters & PARAMETER_E_OFFSET) || heater.getTemperature()) {
						
												// Move
												motors.move(requests[currentProcessingRequest]);
			
												// Set response to confirmation
												strcpy(responseBuffer, "ok");
											}
										
											// Otherwise
											else
										
												// Set response to error
												strcpy(responseBuffer, "Error: Can't use the extruder when the heater is off");
										break;
				
										// G4
										case 4:
										
											{
												// Delay specified number of milliseconds
												int32_t delayTime = requests[currentProcessingRequest].valueP;
												if(delayTime > 0)
													for(int32_t i = 0; i < delayTime; i++)
														delay_ms(1);
												
												// Delay specified number of seconds
												delayTime = requests[currentProcessingRequest].valueS;
												if(delayTime > 0)
													for(int32_t i = 0; i < delayTime; i++)
														delay_s(1);
											}
					
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G28
										case 28:
						
											// Home XY
											motors.homeXY();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G30
										case 30:
						
											// Calibrate bed center Z0
											motors.calibrateBedCenterZ0();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G32
										case 32:
						
											// Calibrate bed orientation
											motors.calibrateBedOrientation();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G33
										case 33:
						
											// Save Z as bed center Z0
											motors.saveZAsBedCenterZ0();
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G90 or G91
										case 90:
										case 91:
						
											// Set mode to absolute
											motors.mode = requests[currentProcessingRequest].valueG == 90 ? ABSOLUTE : RELATIVE;
							
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
										break;
						
										// G92
										case 92:
										
											// Check if an X, Y, Z, or E value is provided
											if(requests[currentProcessingRequest].commandParameters & (PARAMETER_X_OFFSET | PARAMETER_Y_OFFSET | PARAMETER_Z_OFFSET | PARAMETER_E_OFFSET)) {
										
												// Go through all motors
												for(uint8_t i = 0; i < NUMBER_OF_MOTORS; i++) {
												
													// Get parameter offset and value
													uint16_t parameterOffset;
													float *value;
													switch(i) {
													
														case X:
															parameterOffset = PARAMETER_X_OFFSET;
															value = &requests[currentProcessingRequest].valueX;
														break;
														
														case Y:
															parameterOffset = PARAMETER_Y_OFFSET;
															value = &requests[currentProcessingRequest].valueY;
														break;
														
														case Z:
															parameterOffset = PARAMETER_Z_OFFSET;
															value = &requests[currentProcessingRequest].valueZ;
														break;
														
														default:
															parameterOffset = PARAMETER_E_OFFSET;
															value = &requests[currentProcessingRequest].valueE;
													}
													
													// Set parameter is provided
													if(requests[currentProcessingRequest].commandParameters & parameterOffset)
													
														// Set motors current value
														motors.currentValues[i] = *value;
												}
				
												// Set response to confirmation
												strcpy(responseBuffer, "ok");
											}
										break;
										
										// G20 or G21
										case 20:
										case 21:
				
											// Set response to confirmation
											strcpy(responseBuffer, "ok");
									}
								}
								
								// Otherwise check if command has parameter T
								else if(requests[currentProcessingRequest].commandParameters & PARAMETER_T_OFFSET)
				
									// Set response to confirmation
									strcpy(responseBuffer, "ok");
							}
		
							// Check if command has an N parameter and it was processed
							if(requests[currentProcessingRequest].commandParameters & PARAMETER_N_OFFSET && (!strncmp(responseBuffer, "ok", strlen("ok")) || !strncmp(responseBuffer, "rs", strlen("rs")) || !strncmp(responseBuffer, "skip", strlen("skip")))) {
							
								// Check if response is a confirmation and current command number isn't at its max
								if(!strncmp(responseBuffer, "ok", strlen("ok")) && currentCommandNumber != UINT64_MAX)
								
									// Increment current command number
									currentCommandNumber++;
								
								// Append command number to response
								uint8_t endOfResponse = responseBuffer[0] == 's' ? strlen("skip") : strlen("ok");
								ulltoa(responseBuffer[0] == 'r' ? currentCommandNumber : requests[currentProcessingRequest].valueN, numberBuffer);
								memmove(&responseBuffer[endOfResponse + 1 + strlen(numberBuffer)], &responseBuffer[endOfResponse], strlen(responseBuffer) - 1);
								responseBuffer[endOfResponse] = ' ';
								memcpy(&responseBuffer[endOfResponse + 1], numberBuffer, strlen(numberBuffer));
							}
						}
					}
					
					// Check if response wasn't set
					if(!*responseBuffer)
	
						// Set response to error
						strcpy(responseBuffer, "Error: Unknown G-code command");
				}
		
				// Append newline to response
				strcat(responseBuffer, "\n");
		
				// Send response if an emergency stop didn't happen
				if(!emergencyStopOccured)
					sendDataToUsb(responseBuffer);
			}
			
			// Clear request
			requests[currentProcessingRequest].commandParameters = 0;
			
			// Increment current processing request
			currentProcessingRequest = currentProcessingRequest == REQUEST_BUFFER_SIZE - 1 ? 0 : currentProcessingRequest + 1;
			
			// Enable sending wait responses
			enableSendingWaitResponses();
		}
		
		// Otherwise check if an emergency stop has occured
		else if(emergencyStopOccured) {
		
			// Disable sending wait responses
			disableSendingWaitResponses();
		
			// Reset all peripherals
			fan.setSpeed(FAN_MIN_SPEED);
			heater.reset();
			led.setBrightness(LED_MAX_BRIGHTNESS);
			motors.reset();
		
			// Clear emergency stop occured
			emergencyStopOccured = false;
			
			// Send confirmation
			sendDataToUsb("ok\n");
			
			// Enable sending wait responses
			enableSendingWaitResponses();
		}
	}
	
	// Return
	return EXIT_SUCCESS;
}


// Supporting function implementation
void cdcRxNotifyCallback(uint8_t port) {

	// Initialize variables
	static uint8_t currentReceivingRequest = 0;
	static uint8_t lastCharacterOffset = 0;
	static char accumulatedBuffer[UINT8_MAX + 1];
	
	// Get request
	uint8_t size = udi_cdc_get_nb_received_data();
	char buffer[UDI_CDC_COMM_EP_SIZE];
	udi_cdc_read_buf(buffer, size);
	
	// Prevent request from overflowing accumulated request
	if(size + lastCharacterOffset >= sizeof(accumulatedBuffer))
		size = sizeof(accumulatedBuffer) - lastCharacterOffset - 1;
	buffer[size] = 0;
	
	// Accumulate requests
	strcpy(&accumulatedBuffer[lastCharacterOffset], buffer);
	lastCharacterOffset += size;
	
	// Check if no more data is available
	if(size != UDI_CDC_COMM_EP_SIZE) {
	
		// Clear last character offset
		lastCharacterOffset = 0;
	
		// Check if an emergency stop isn't being processed
		if(!emergencyStopOccured) {
	
			// Go through all commands in request
			for(char *offset = accumulatedBuffer; *offset;) {
	
				// Parse request
				Gcode gcode;
				gcode.parseCommand(offset);
	
				// Check if request is an emergency stop and it has a valid checksum if it has an N parameter
				if(gcode.commandParameters & PARAMETER_M_OFFSET && !gcode.valueM && (!(gcode.commandParameters & PARAMETER_N_OFFSET) || gcode.commandParameters & VALID_CHECKSUM_OFFSET)) {

					// Stop all peripherals
					heater.emergencyStopOccured = motors.emergencyStopOccured = emergencyStopOccured = true;
				
					// Break
					break;
				}

				// Otherwise check if currently receiving request isn't empty
				else if(!requests[currentReceivingRequest].commandParameters) {
		
					// Set current receiving request to command
					requests[currentReceivingRequest] = gcode;
			
					// Increment current receiving request
					currentReceivingRequest = currentReceivingRequest == REQUEST_BUFFER_SIZE - 1 ? 0 : currentReceivingRequest + 1;
				}
			
				// Go to next command
				if(*(offset = strchr(offset, '\n')))
					offset++;
			}
		}
	}
}

void cdcDisconnectCallback(uint8_t port) {

	// Prepare to reattach to the host
	udc_detach();
	udc_attach();
}

void disableSendingWaitResponses() {

	// Disable sending wait responses
	tc_set_overflow_interrupt_level(&WAIT_TIMER, TC_INT_LVL_OFF);
}

void enableSendingWaitResponses() {

	// Reset wait timer counter
	waitTimerCounter = 0;
	
	// Enable sending wait responses
	tc_set_overflow_interrupt_level(&WAIT_TIMER, TC_INT_LVL_LO);
}
