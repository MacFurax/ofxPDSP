
#include "OscInput.h"

#define OFXPDSP_OSCINPUT_MESSAGERESERVE 128
#define OFXPDSP_OSCCIRCULARBUFFER_SIZE 10000

pdsp::osc::Input::OscChannel::OscChannel(){
    
    key = "";
    messageBuffer = nullptr;
    gate_out = nullptr;
    value_out = nullptr;
    argument = 0;
    
}

void pdsp::osc::Input::OscChannel::deallocate(){
    if( messageBuffer != nullptr ){
        delete messageBuffer;
    }
    if( gate_out != nullptr ) {
        delete gate_out;
    }
    if ( value_out != nullptr ) {
        delete value_out;
    }            
}


pdsp::osc::Input::Input() {
    
    oscChannels.clear();
    
    readVector.reserve(OFXPDSP_OSCINPUT_MESSAGERESERVE);
    circularBuffer.resize( OFXPDSP_OSCCIRCULARBUFFER_SIZE );
    
    lastread = 0;
    index = 0;
    
    connected = false;
    
    runDaemon = false;
    daemonRefreshRate = 500;
}   


pdsp::osc::Input::~Input(){
    if(connected){
        close();
    }
    
    for (size_t i = 0; i < oscChannels.size(); ++i){
        oscChannels[i]->deallocate();
        delete oscChannels[i];
        oscChannels[i] = nullptr;
    }
}

void pdsp::osc::Input::setVerbose( bool verbose ){
    this->verbose = verbose;
}


void pdsp::osc::Input::openPort( int port ) {
    if(connected){
        close();
    }
    
    receiver.setup( port );
    
    startDaemon();
    
    connected = true;
    bufferChrono = chrono::high_resolution_clock::now();
}



void pdsp::osc::Input::close(){
    if(connected){
        if(verbose) cout<<"[pdsp] shutting down OSC out\n";
        //stop the daemon before
        closeDaemon();

        connected = false;        
    }
}



pdsp::SequencerGateOutput& pdsp::osc::Input::out_trig( string oscAddress, int argument ) {
    
    for ( OscChannel* & osc : oscChannels ){
        if( osc->key == oscAddress && osc->argument == argument ) {
            if( osc->gate_out != nullptr ){
                return *(osc->gate_out);
            }else{
                cout<<"[pdsp] warning! this osc string and argument was already used as value output, returning dummy gate output\n";
                pdsp::pdsp_trace();
                return invalidGate;
            }
        }
    }
    
    // not found
    OscChannel* osc = new OscChannel();
    osc->key = oscAddress;
    osc->argument = argument;
    //osc->mode = Gate;
    osc->messageBuffer = new pdsp::MessageBuffer();
    osc->gate_out = new pdsp::SequencerGateOutput();
    osc->gate_out->link( *(osc->messageBuffer) );
    oscChannels.push_back(osc);

    return *(osc->gate_out);
}


pdsp::SequencerValueOutput& pdsp::osc::Input::out_value( string oscAddress, int argument ) {
   
    for ( OscChannel* & osc : oscChannels ){
        if( osc->key == oscAddress  && osc->argument == argument ) {
            if( osc->value_out != nullptr ){
                return *(osc->value_out);
            }else{
                cout<<"[pdsp] warning! this osc string and argument was already used as gate output, returning dummy value output\n";
                pdsp::pdsp_trace();
                return invalidValue;
            }
        }
    }

    // not found
    OscChannel* osc = new OscChannel();
    osc->key = oscAddress;
    osc->argument = argument;
    //osc->mode = Value;
    osc->messageBuffer = new pdsp::MessageBuffer();
    osc->value_out = new pdsp::SequencerValueOutput();
    osc->value_out->link( *(osc->messageBuffer) );
    oscChannels.push_back(osc);

    return *(osc->value_out);    

}

void pdsp::osc::Input::clearAll(){
    sendClearMessages = true;
}

void pdsp::osc::Input::prepareToPlay( int expectedBufferSize, double sampleRate ){
    oneSlashMicrosecForSample = 1.0 / (1000000.0 / sampleRate);
}

void pdsp::osc::Input::releaseResources(){}



void pdsp::osc::Input::daemonFunction() noexcept{
    
    while (runDaemon){
        
        while(receiver.hasWaitingMessages()){
            
            ofxOscMessage osc;
            receiver.getNextMessage(osc);
            
            // calculate the right offset inside the bufferSize
            int write = index +1;
            if(write>=(int)circularBuffer.size()){ write = 0; } 

            circularBuffer[write].message = osc;
            circularBuffer[write].timepoint = std::chrono::high_resolution_clock::now();

            index = write;    

        }

        this_thread::sleep_for(std::chrono::microseconds(daemonRefreshRate));

    }
   
    if(verbose) cout<<"[pdsp] closing OSC input daemon thread\n";
}
    

void pdsp::osc::Input::pushToReadVector( pdsp::osc::Input::_PositionedOscMessage & message ){
        std::chrono::duration<double> offset = message.timepoint - bufferChrono; 
        message.sample = static_cast<int>( static_cast <double>( std::chrono::duration_cast<std::chrono::microseconds>(offset).count()) * oneSlashMicrosecForSample);
        readVector.push_back( message );    
}

void pdsp::osc::Input::processOsc( int bufferSize ) noexcept {
    
    if(connected){
        
        readVector.clear();
        int read = index;

        if(read<lastread){ // two segments
            for(int i=lastread+1; i<(int)circularBuffer.size(); ++i){
                pushToReadVector( circularBuffer[i] );
            }            
            for( int i=0; i<=read; ++i){
                pushToReadVector( circularBuffer[i] );
            }            
        }else{
            for(int i=lastread+1; i<=read; ++i){
                pushToReadVector( circularBuffer[i] );
            }
        }
        
        lastread = read;
        bufferChrono = std::chrono::high_resolution_clock::now();
        
        //now sanitize messages to bufferSize
        for(_PositionedOscMessage &msg : readVector){
            if(msg.sample >= bufferSize){ msg.sample = bufferSize-1; } else
            if(msg.sample < 0 ) { msg.sample = 0; }
        }
        
        // clean the message buffers
        for (size_t i = 0; i < oscChannels.size(); ++i){
            oscChannels[i]->messageBuffer->clearMessages();

            if(sendClearMessages){
                oscChannels[i]->messageBuffer->addMessage(0.0f, 0);
            }
        }
        
        // adds the messages to the buffers, only the first arg of each osc message is read, as float
        for(_PositionedOscMessage &osc : readVector){
            for (size_t i = 0; i < oscChannels.size(); ++i){
                
                if(osc.message.getAddress() == oscChannels[i]->key && oscChannels[i]->argument < int(osc.message.getNumArgs()) ){
                    
                    switch( osc.message.getArgType(oscChannels[i]->argument) ){
                        case OFXOSC_TYPE_INT32:
                            oscChannels[i]->messageBuffer->addMessage( osc.message.getArgAsInt32(oscChannels[i]->argument), osc.sample );
                        break;
                        
                        case OFXOSC_TYPE_FLOAT:
                            oscChannels[i]->messageBuffer->addMessage( osc.message.getArgAsFloat(oscChannels[i]->argument), osc.sample );
                        break;

                        case OFXOSC_TYPE_TRUE:
                            oscChannels[i]->messageBuffer->addMessage( 1.0f, osc.sample );
                        break;
                        
                        case OFXOSC_TYPE_FALSE:
                            oscChannels[i]->messageBuffer->addMessage( 0.0f, osc.sample );
                        break;
                        
                        case OFXOSC_TYPE_STRING:
                        {   // try to parse string
                            float number = ofToFloat(osc.message.getArgAsString(oscChannels[i]->argument));
                            oscChannels[i]->messageBuffer->addMessage( number, osc.sample );
                        }
                        break;
                        
                        default: break;
                    }
                }
            }
        }
        
        // now process all the linked sequencers
        for (size_t i = 0; i < oscChannels.size(); ++i){
            oscChannels[i]->messageBuffer->processDestination(bufferSize);
        }
        
    }
    
}


void pdsp::osc::Input::startDaemon(){ // OK
    if(verbose) cout<<"[pdsp] starting OSC input daemon\n";
    runDaemon = true;
    daemonThread = thread( daemonFunctionWrapper, this );   
    
}


void pdsp::osc::Input::closeDaemon(){
 
    runDaemon = false;
    daemonThread.detach();

}
        
void pdsp::osc::Input::daemonFunctionWrapper(pdsp::osc::Input* parent){
    parent->daemonFunction();
}
