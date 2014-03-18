
#include <Skynetclient.h>

#define TOKEN_STRING(js, t, s) \
	(strncmp(js+(t).start, s, (t).end - (t).start) == 0 \
	 && strlen(s) == (t).end - (t).start)
		 
struct tx_buffer
{
  unsigned char buffer[SKYNET_TX_BUFFER_SIZE];
  volatile unsigned int head;
  volatile unsigned int tail;
};

struct rx_buffer
{
  unsigned char buffer[SKYNET_RX_BUFFER_SIZE];
  volatile unsigned int head;
  volatile unsigned int tail;
};

rx_buffer socket_rx_buffer =    { { 0 }, 0, 0};
tx_buffer socket_tx_buffer =    { { 0 }, 0, 0};

inline void store_char(unsigned char c, rx_buffer *buffer)
{
  int i = (unsigned int)(buffer->head + 1) % SKYNET_RX_BUFFER_SIZE;

  // if we should be storing the received character into the location
  // just before the tail (meaning that the head would advance to the
  // current location of the tail), we're about to overflow the buffer
  // and so we don't write the character or advance the head.
  if (i != buffer->tail) {
    buffer->buffer[buffer->head] = c;
    buffer->head = i;
  }
}

SkynetClient::SkynetClient(Client &_client){
	this->client = &_client; 
}

int SkynetClient::connect(const char* host, uint16_t port) {
	_rx_buffer = &socket_rx_buffer;
	_tx_buffer = &socket_tx_buffer;
	lastReceivedUuid[0]=0;
	thehost = host;

	//connect tcp or fail
	if (!client->connect(host, port)) 
		return false;

	//establish socket or fail
	sendHandshake();
	if(!readHandshake()){
		stop();
		return false;
	}
	
	//monitor to initiate communications with skynet TODO some fail condition
	while(!status)
		monitor();
	
	return status;
}

int SkynetClient::connect(IPAddress ip, uint16_t port) {
	return -1;
}

uint8_t SkynetClient::connected() {
  return status;
}

void SkynetClient::stop() {
	status = 0;
	client->stop();
}

//dump until peek is not a colon
void SkynetClient::dump(){
	if(!client->available())
		return;
	
	DBGC(F("Dumping: "));
	do{
		char c = client->read();
		DBGC(c);
	}while ( client->available() && client->peek() == ':' );
	DBGCN("");
}

//eg on first connect:
// 1::ÿ 5:::{"name":"identify","args":[{"socketid":"zb2gh9TSVZ-eyUf8YZ_4"}]}ÿ
// or after  2::ÿ or:
// 5:::{"name":"identify","args":[{"socketid":"zb2gh9TSVZ-eyUf8YZ_4"}]}ÿ
void SkynetClient::monitor() 
{
	flushTX();

	char which = 0;
	
	if(client->available()){
		which = client->read();
	}else{
		return;
	}
	
	switch (which) {
		//disconnect	
		case '0':
			DBGCN(F("Disconnect"));
			stop();
			break;
		
		//messages
		case '1':
			DBGCN(F("Socket Connect"));
			break;
			
		case '3':	
		case '5':	
			DBGCN(F("Message"));
			dump();
			process();
			break;
			
		//hearbeat
		case '2':		
			client->print((char)0);
			client->print(F("2::"));
			client->print((char)255);
			DBGCN(F("Heartbeat"));
			break;

	    //huh?
		default:
			DBGC(F("Drop: "));
			DBGCN(which);
			break;
	}
}

void SkynetClient::process()
{
	DBGCN(F("Processing Message"));
    if (client->available()) {
	  
	int size = readLine();

	jsmn_parser p;
	jsmntok_t tok[MAX_PARSE_OBJECTS];

	jsmn_init(&p);

	int r = jsmn_parse(&p, databuffer, tok, MAX_PARSE_OBJECTS);
	if (r != 0){
	    DBGCN(F("parse failed"));
		DBGCN(r);
		return;
	}

    if (TOKEN_STRING(databuffer, tok[2], IDENTIFY )) 
    {
		DBGCN(F(IDENTIFY));
	    DBGC(F("Sending: "));

	    DBGC((char)0);
		client->print((char)0);

	    DBGC(EMIT);	
		client->print(EMIT);
	
		printByByte("{\"name\":\"identity\",\"args\":[{\"socketid\":\"");
		printToken(databuffer, tok[7]);
		
		if( eeprom_read_byte( (uint8_t*)EEPROMBLOCKADDRESS) == EEPROMBLOCK )
		{
			eeprom_read_bytes(token, TOKENADDRESS, TOKENSIZE);
			token[TOKENSIZE-1]='\0'; //in case courrupted or not defined
	
			eeprom_read_bytes(uuid, UUIDADDRESS, UUIDSIZE);
			uuid[UUIDSIZE-1]='\0'; //in case courrupted or not defined

			printByByte("\", \"uuid\":\"");
			printByByte(uuid);

			printByByte("\", \"token\":\"");
			printByByte(token);
		}
		printByByte("\"}]}");
	  
		DBGCN((char)255);
		client->print((char)255);
    } 
    else if (TOKEN_STRING(databuffer, tok[2], READY )) 
    {
		DBGCN(READY);
		status = 1;
		
        eeprom_read_bytes(token, TOKENADDRESS, TOKENSIZE);
        token[TOKENSIZE-1]='\0'; //in case courrupted or not defined
		
        //if token has been refreshed, save it
        if (!TOKEN_STRING(databuffer, tok[15], token ))
        {
			DBGCN(F("token refresh"));
		  	strncpy(token, databuffer + tok[15].start, tok[15].end - tok[15].start);

            DBGC(F("new: "));
          	DBGCN(token);
          
		  	eeprom_write_bytes(token, TOKENADDRESS, TOKENSIZE);

			//write block identifier, arduino should protect us from writing if it doesnt need it?
          	eeprom_write_byte((uint8_t *)EEPROMBLOCKADDRESS, (uint8_t)EEPROMBLOCK); 

        }else
        {
			DBGCN(F("no token refresh necessary"));
        }
		
		eeprom_read_bytes(uuid, UUIDADDRESS, UUIDSIZE);
        uuid[UUIDSIZE-1]='\0'; //in case courrupted or not defined

        //if uuid has been refreshed, save it
        if (!TOKEN_STRING(databuffer, tok[13], uuid ))
        {
          	DBGCN(F("uuid refresh"));
			strncpy(uuid, databuffer + tok[13].start, tok[13].end - tok[13].start);
			
          	DBGC(F("new: "));
          	DBGCN(uuid);
			
          	eeprom_write_bytes(uuid, UUIDADDRESS, UUIDSIZE);
			
			//write block identifier, arduino should protect us from writing if it doesnt need it?
          	eeprom_write_byte((uint8_t *)EEPROMBLOCKADDRESS, (uint8_t)EEPROMBLOCK); 

         }else
         {
           	DBGCN(F("no uuid refresh necessary"));
         }
		 
    }
    else if (TOKEN_STRING(databuffer, tok[2], NOTREADY )) 
    {
		//send blank identify
		DBGCN(NOTREADY);

		printByByte("{\"name\":\"identity\",\"args\":[{\"socketid\":\"");
		printToken(databuffer, tok[7]);
		printByByte("\"}]}");
    }
    else if (TOKEN_STRING(databuffer, tok[2], MESSAGE )) 
    {
		DBGCN(MESSAGE);
				
		DBGCN("Storing: ");
	    for(int i = tok[9].start; i < tok[9].end; i++) 
	    {
			DBGC(databuffer[i]);
        	store_char( databuffer[i], &socket_rx_buffer);
	    }
		
		DBGCN("lastreceiveduuid: ");
	    for(int i = tok[13].start; i < tok[13].end; i++) 
	    {
			DBGC(databuffer[i]);
	    }
		
	    strncpy(lastReceivedUuid, databuffer + tok[13].start, tok[13].end - tok[13].start);

  		lastReceivedUuid[UUIDSIZE] = '\0'; //place the null char

		DBGCN();
		DBGC("stored uuid: ");
		DBGCN(lastReceivedUuid);

		if (messageDelegate != NULL) {
			messageDelegate(databuffer);
		}
    }
    else
    {
		DBGC(F("Unknown:"));
    }
  }
}

void SkynetClient::sendHandshake() {
	client->println(F("GET /socket.io/1/ HTTP/1.1"));
	client->print(F("Host: "));
	client->println(thehost);
	client->println(F("Origin: Arduino\r\n"));
}

bool SkynetClient::waitForInput(void) {
	unsigned long now = millis();
	while (!client->available() && ((millis() - now) < 30000UL)) {;}
	return client->available();
}

void SkynetClient::eatHeader(void) {
	while (client->available()) {	// consume the header
		readLine();
		if (strlen(databuffer) == 0) break;
	}
}

int SkynetClient::readHandshake() {

	if (!waitForInput()) return false;

	// check for happy "HTTP/1.1 200" response
	readLine();
	if (atoi(&databuffer[8]) != 200) {
		while (client->available()) readLine();
		client->stop();
		return 0;
	}
	eatHeader();
	readLine();	// read first line of response
	readLine();	// read sid : transport : timeout
		
	char sid[SID_LEN];
	char *iptr = databuffer;
	char *optr = sid;
	while (*iptr && (*iptr != ':') && (optr < &sid[SID_LEN-2])) *optr++ = *iptr++;
	*optr = 0;

	DBGC(F("Connected. SID="));
	DBGCN(sid);	// sid:transport:timeout 

	while (client->available()) readLine();

	client->print(F("GET /socket.io/1/websocket/"));
	client->print(sid);
	client->println(F(" HTTP/1.1"));
	client->print(F("Host: "));
	client->println(thehost);
	client->println(F("Origin: ArduinoSkynetClient"));
	client->println(F("Upgrade: WebSocket"));	// must be camelcase ?!
	client->println(F("Connection: Upgrade\r\n"));

	if (!waitForInput()) return 0;

	readLine();
	if (atoi(&databuffer[8]) != 101) {
		while (client->available()) readLine();
		client->stop();
		return false;
	}
	eatHeader();
	monitor();		// treat the response as input
	return 1;
}

int SkynetClient::readLine() {
	int numBytes = 0;
	dataptr = databuffer;
	DBGC(F("Readline: "));
	while (client->available() && (dataptr < &databuffer[SOCKET_RX_BUFFER_SIZE-2])) {
		char c = client->read();
		if (c == 0){
			;
		}else if (c == -1){
			;
		}else if (c == 255){
			;
		}else if (c == '\r') {
			;
		}else if (c == '\n') 
			break;
		else {
			DBGC(c);
			*dataptr++ = c;
			numBytes++;
		}
	}
	DBGCN();
	*dataptr = 0;
	return numBytes;
}

//wifi client->print has a buffer that so far we've been unable to locate
//under 154 (our identify size) for sure.. so sending char by char for now
void SkynetClient::printByByte(const char *data, size_t size) {
	if(data != NULL && data[0] != '\0')
	{
		int i = 0;
		while ( i < size)
		{
		    DBGC(data[i]);
			client->print(data[i++]);
		}
	}
}

//wifi client->print has a buffer that so far we've been unable to locate
//under 154 (our identify size) for sure.. so sending char by char for now
void SkynetClient::printByByte(const char *data) {
	if(data != NULL)
	{
		int i = 0;
		while ( data[i] != '\0' )
		{
		    DBGC(data[i]);
			client->print(data[i++]);
		}
	}
}

void SkynetClient::printToken(const char *js, jsmntok_t t) 
{
	int i = 0;
	for(i = t.start; i < t.end; i++) {
	    DBGC(js[i]);
		client->print(js[i]);
	 }
}

size_t SkynetClient::writeRaw(const uint8_t *buf, size_t size) {
    DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);
	
	printByByte(reinterpret_cast<const char*>(buf), size);

    DBGCN((char)255);
	client->print((char)255);

	return size;
}

size_t SkynetClient::write(const uint8_t *buf, size_t size) 
{
    DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);
		
	printByByte("{\"name\":\"message\",\"args\":[{\"devices\":\"");
	printByByte(lastReceivedUuid);
	printByByte("\",\"payload\":\"");
	printByByte(reinterpret_cast<const char*>(buf), size);
	printByByte("\"}]}");

    DBGCN((char)255);
	client->print((char)255);
}

//place write data into a buffer to be sent on next flush or monitor
size_t SkynetClient::write(uint8_t c)
{
	int i = (_tx_buffer->head + 1) % SKYNET_TX_BUFFER_SIZE;

  	_tx_buffer->buffer[_tx_buffer->head] = c;
	_tx_buffer->head = i;

	return 1;
}

int SkynetClient::available() {
  return (unsigned int)(SKYNET_RX_BUFFER_SIZE + _rx_buffer->head - _rx_buffer->tail) % SKYNET_RX_BUFFER_SIZE;
}

int SkynetClient::read() {
    // if the head isn't ahead of the tail, we don't have any characters
    if (_rx_buffer->head == _rx_buffer->tail) {
      return -1;
    } else {
      unsigned char c = _rx_buffer->buffer[_rx_buffer->tail];
      _rx_buffer->tail = (unsigned int)(_rx_buffer->tail + 1) % SKYNET_RX_BUFFER_SIZE;
      return c;
    }
}

// //TODO	
// int SkynetClient::read(uint8_t *buf, size_t size) {
// }

int SkynetClient::peek() {
    if (_rx_buffer->head == _rx_buffer->tail) {
      return -1;
    } else {
      return _rx_buffer->buffer[_rx_buffer->tail];
    }
}

void SkynetClient::flush() 
{
	client->flush();
}

void SkynetClient::flushTX(){
	if(_tx_buffer->head != _tx_buffer->tail && strlen(lastReceivedUuid)!=0){
		DBGC(F("Sending: "));

	    DBGC((char)0);
		client->print((char)0);

	    DBGC(EMIT);	
		client->print(EMIT);
			
		printByByte("{\"name\":\"message\",\"args\":[{\"devices\":\"");
		printByByte(lastReceivedUuid);
		printByByte("\",\"payload\":\"");
		
		while(_tx_buffer->head != _tx_buffer->tail)
		{
			DBGC((char)(_tx_buffer->buffer[_tx_buffer->tail]));
			client->print(_tx_buffer->buffer[_tx_buffer->tail]);
      		_tx_buffer->tail = (unsigned int)(_tx_buffer->tail + 1) % SKYNET_TX_BUFFER_SIZE;
		}

		printByByte("\"}]}");

		DBGCN((char)255);
		client->print((char)255);
	}
}

// the next function allows us to use the client returned by
// SkynetClient::available() as the condition in an if-statement.

SkynetClient::operator bool() {
  return true;
}

void SkynetClient::setMessageDelegate(MessageDelegate newMessageDelegate) {
	  messageDelegate = newMessageDelegate;
}

void SkynetClient::eeprom_write_bytes(char *buf, int address, int bufSize){
  for(int i = 0; i<bufSize; i++){
    EEPROM.write(address+i, buf[i]);
  }
}

void SkynetClient::eeprom_read_bytes(char *buf, int address, int bufSize){
  for(int i = 0; i<bufSize; i++){
    buf[i] = EEPROM.read(address+i);
  }
}

void SkynetClient::sendMessage(const char *object)
{
	sendMessage(lastReceivedUuid, object);
}

void SkynetClient::sendMessage(const char *device, const char *object)
{
	DBGC(F("Sending: "));

    DBGC((char)0);
	client->print((char)0);

    DBGC(EMIT);	
	client->print(EMIT);

	printByByte("{\"name\":\"message\",\"args\":[{\"devices\":\"");
	printByByte(device);
	printByByte("\",\"payload\":\"");
	printByByte(object);
	printByByte("\"}]}");

	DBGCN((char)255);
	client->print((char)255);
}