# cTrader FIX engine

## System messages

### Heartbeat (Client ↔ cTrader)
### Test Request (Client ↔ cTrader)
### Logon (Client → cTrader)
### Logout (Client → cTrader)
### Resend Request (Client ↔ cTrader)
### Reject (Client ↔ cTrader)
### Sequence Reset (Client ↔ cTrader)

## Application messages

### Market Data Request (Client → cTrader)
### Market Data Snapshot/Full Refresh (Client ← cTrader)
### Market Data Incremental Refresh (Client ← cTrader)
### New Order Single (Client → cTrader)
### Order Status Request (Client → cTrader)
### Order Mass Status Request (Client → cTrader)
### Execution Report (Client ← cTrader)
### Business Message Reject (Client ← cTrader)
### Request for Positions (Client → cTrader)
### Position Report (Client ← cTrader)
### Order Cancel Request (Client → cTrader)
### Order Cancel Reject (Client ← cTrader)
### Order Cancel/Replace Request (Client → cTrader)
### Security List Request (Client → cTrader)
### Security List (Client ← cTrader)

## Standard header

Each administrative or application message is preceded by a standard header. Headers identify a message type, length, destination, sequence number, origination point and time.

All messages sent to cTrader should have a standard header with the following fields:

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| 8 | BeginString | Yes | FIX.4.4 | String | Always unencrypted, must be the first field in a message. |
| 9 | BodyLength | Yes | Any valid value | Integer | Message body length. Always unencrypted, must be the second field in a message. |
| 35 | MsgType | Yes | A | String | A message type. Always unencrypted, must be the third field in a message. |
| 49 | SenderCompID | Yes | Any valid value | String | An ID of the trading party in the following format: <Environment>.<BrokerUID>.<Trader Login>, where Environment is a determination of the server, like demo or live; BrokerUID is provided by cTrader and Trader Login is a numeric identifier of the trader account. |
| 56 | TargetCompID | Yes | CSERVER | String | A message target. The valid value is CSERVER. |
| 57 | TargetSubID | Yes | QUOTE or TRADE | String | An additional session qualifier. Possible values are QUOTE and TRADE. |
| 50 | SenderSubID | No | Any valid value | String | The assigned value used to identify a specific message originator. Must be set to QUOTE if TargetSubID=QUOTE. |
| 34 | MsgSeqNum | Yes | 1 | Integer | A sequence number of the message. |
| 52 | SendingTime | Yes | 20131129-15:40:08.155 | UTCTimestamp | Time of the message transmission always expressed in UTC (Universal Time Coordinated, also known as GMT). |

## Standard trailer

Each message, administrative or application, is terminated by a standard trailer. The trailer is used to segregate messages and contains a three-digit representation of the CheckSum (tag=10) value.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| 10 | CheckSum | Yes | 054 | String | A three-byte simple checksum. Always the last field in a message (for example, serves), with the trailing <SOH> as the end-of-message delimiter. It is defined as three characters (always unencrypted). |

## Session messages

### Heartbeat (MsgType(35)=0)

Heartbeat messages are sent by both cTrader and the client application to confirm a live connection.

The provider’s client application transmits a recurring heartbeat at the interval, which is defined by the HeartBtInt (tag=108) field in a Logon message, or as a response to a Test Request message.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 112 | TestReqID | No | Any valid value | String | If a heartbeat is a result of the Test Request message, TestReqID is required. |
| Standard Trailer | Yes | | | | |

### Test Request (MsgType(35)=1)

It forces a heartbeat from the receiver of a request. A response is sent from the receiving system as a Heartbeat message containing TestReqID.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 112 | TestReqID | Yes | Any valid value | String | A heartbeat message ID. TestReqID should be incremental. |
| Standard Trailer | Yes | | | | |

### Logon (bidirectional) (MsgType(35)=A)

A Logon message is sent from the client side application to begin a cTrader FIX session, and a response is sent by cTrader to the client side application. Once the logon is complete, quote and trade flows can proceed for the lifecycle of the session.

If an invalid Logon message is received by cTrader (with invalid fields), cTrader sends a Logout message in response.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 98 | EncryptMethod | Yes | 0 | Integer | Defines a message encryption scheme. Currently, only transport-level security is supported. The valid value is 0 = NONE_OTHER (encryption is not used). |
| 108 | HeartBtInt | Yes | Any valid value | Integer | A heartbeat interval in seconds. The value is set in the config.properties file (client side) as SERVER.POLLING.INTERVAL. The default interval value is 30 seconds. If HeartBtInt is set to 0, no heartbeat message is required. |
| 141 | ResetSeqNumFlag | No | Y | Boolean | All sides of the FIX session should have the sequence numbers reset. The valid value is Y (reset). |
| 553 | Username | No | Any valid value | String | A numeric User ID. The user is linked to the SenderCompID value (the user’s organization, tag=49). |
| 554 | Password | No | Any valid value | String | A user password. |
| Standard Trailer | Yes | | | | |

**Note**

The field Username (tag=553) must contain a numeric trader login value, whilst SenderCompID (tag=49) must contain an environment, BrokerUID and a trader login delimited by a dot (for example, live.theBroker.12345).

See examples of Logon messages below.

Request:
`8=FIX.4.4|9=126|35=A|49=live.theBroker.12345|56=CSERVER|34=1|52=20170117-08:03:04|57=TRADE|50=any_string|98=0|108=30|141=Y|553=12345|554=passw0rd!|10=131|`

Response (success):
`8=FIX.4.4|9=106|35=A|34=1|49=CSERVER|50=TRADE|52=20170117-08:03:04.509|56=live.theBroker.12345|57=any_string|98=0|108=30|141=Y|10=066|`

Response (failed):
`8=FIX.4.4|9=109|35=5|34=1|49=CSERVER|50=TRADE|52=20170117-08:03:04.509|56=live.theBroker.12345|58=InternalError: RET_INVALID_DATA|10=033|`

### Logout (MsgType(35)=5)

A Logout message is sent from the client application to request a session end with cTrader and as a response by cTrader. A session logout occurs in response to a Market Participant sending a Logout message to cTrader. Before terminating the session, cTrader will cancel all prices that are still actively streaming out to the requesting party. If an invalid Logon message is received by cTrader (with invalid fields), cTrader sends a Logout message in response with error details in the Text (tag=58) field.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 58 | Text | No | Any valid value | String | Logon rejection details. Used only for cTrader-to-client messages as an invalid Logon message response. |
| Standard Trailer | Yes | | | | |

See examples of Logout messages below.

Request:
`8=FIX.4.4|9=86|35=5|49=live.theBroker.12345|56=CSERVER|34=161|52=20170117-09:22:33|57=TRADE|50=any_string|10=102|`

Response:
`8=FIX.4.4|9=90|35=5|34=160|49=CSERVER|50=TRADE|52=20170117-09:22:33.077|56=live.theBroker.12345|57=any_string|10=044|`

### Resend Request (MsgType(35)=2)

An inbound/outbound message is used to request resending a message (or messages), typically when a gap is detected in the sequence numbering.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 7 | BeginSeqNo | Yes | Any valid value | Integer | A message sequence number of the first record in the range to be resent. |
| 16 | EndSeqNo | Yes | Any valid value | Integer | A message sequence number of the last record in the range to be resent. |
| Standard Trailer | Yes | | | | |

### Reject (bidirectional) (MsgType(35)=3)

Sent when the received message cannot be processed due to a session-level rule violation. Refused messages must be recorded and an increment must be applied to the incoming sequence number.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 45 | RefSeqNum | Yes | Any valid value | SeqNum | A sequence number of the referenced message. |
| 58 | Text | No | Any valid value | String | A free-format text string. |
| 354 | EncodedTextLen | No | Any valid value | Length | Length of the EncodedText (non-ASCII characters) field in bytes. |
| 355 | EncodedText | No | Any valid value | Data | A representation of the Text (tag=58) field, encoded using the format specified in the MessageEncoding (tag=347) field (from the standard header). If the ASCII representation is used, it should also be specified in the Text (tag=58) field. |
| 371 | RefTagID | No | Any valid value | Integer | A tag number of the FIX field that initiated the message refusal. |
| 372 | RefMsgType | No | Any valid value | String | MsgType (tag=35) of the referenced FIX message. |
| 373 | SessionRejectReason | No | Any valid value | Integer | Coded causes of the rejection. The valid values are:<br>0 = Invalid tag number<br>1 = Missing required tag<br>2 = No tag defined for this message type<br>3 = Undefined Tag<br>4 = No value for specified tag<br>5 = Value for this tag is out of range<br>6 = Incorrect data format for value<br>7 = Decryption problem<br>8 = Signature error<br>9 = CompID error<br>10 = SendingTime accuracy error<br>11 = MsgType invalid<br>12 = XML Validation error<br>13 = Tag is being repeated<br>14 = Specified tag is not in correct order<br>15 = Repeating group fields not in correct order<br>16 = Incorrect NumInGroup count for repeating group<br>17 = Field delimiter (SOH character) included in non data value |
| Standard Trailer | Yes | | | | |

### Sequence Reset (MsgType(35)=4)

An inbound/outbound message should not be used at an application level. A Sequence Reset message can only increase a sequence number.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 123 | GapFillFlag | No | Yes or No | String | Indicates that a Sequence Reset message is replacing administrative or application messages which will not be resent. |
| 36 | NewSeqNo | Yes | 1 | Integer | A new sequence number. |
| Standard Trailer | Yes | | | | |

## Application messages

### Market Data Request (MsgType(35)=V)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 262 | MDReqID | Yes | Any valid value | String | A unique quote request ID. A new ID for a new subscription, the same ID as used before for a subscription removal. |
| 263 | SubscriptionRequestType | Yes | 1 or 2 | Char | 1 = Snapshot plus updates (subscribe).<br>2 = Disable previous snapshot plus update request (unsubscribe). |
| 264 | MarketDepth | Yes | 0 or 1 | Integer | A full book will be provided.<br>0 = Depth subscription<br>1 = Spot subscription |
| 265 | MDUpdateType | Yes | Any valid value | Integer | Only the Incremental Refresh is supported. |
| 267 | NoMDEntryTypes | Yes | 2 | Integer | Always set to 2 (both bid and ask will be sent). |
| 269 | MDEntryType | Yes | 0 or 1 | Char | This repeating group contains a list of all types of the Market Data Entries the requester wants to receive.<br>0 = Bid<br>1 = Offer |
| 146 | NoRelatedSym | Yes | Any valid value | Integer | The number of symbols requested. |
| 55 | Symbol | Yes | Any valid value | Long | Instrument identifiers are provided by Spotware. |
| Standard Trailer | Yes | | | | |

See examples of Market Data Request messages below.

For Spots

Request:
`8=FIX.4.4|9=131|35=V|49=live.theBroker.12345|56=CSERVER|34=3|52=20170117-10:26:54|50=QUOTE|262=876316403|263=1|264=1|265=1|146=1|55=1|267=2|269=0|269=1|10=094|`

Response:
`8=FIX.4.4|9=134|35=W|34=2|49=CSERVER|50=QUOTE|52=20170117-10:26:54.630|56=live.theBroker.12345|57=any_string|55=1|268=2|269=0|270=1.06625|269=1|270=1.0663|10=118|`

For Depths

Request:
`8=FIX.4.4|9=131|35=V|49=live.theBroker.12345|56=CSERVER|34=2|52=20170117-11:13:44|50=QUOTE|262=876316411|263=1|264=0|265=1|146=1|55=1|267=2|269=0|269=1|10=087|`

Responses:
`8=FIX.4.4|9=310|35=W|34=2|49=CSERVER|50=QUOTE|52=20180925-12:05:28.284|56=live.theBroker.12345|57=Quote|55=1|268=6|269=1|270=1.11132|271=3000000|278=16|269=1|270=1.11134|271=5000000|278=17|269=1|270=1.11133|271=3000000|278=15|269=0|270=1.1112|271=2000000|278=12|269=0|270=1.11121|271=1000000|278=13|269=0|270=1.11122|271=3000000|278=14|10=247|`

`8=FIX.4.4|9=693|35=X|34=2|49=CSERVER|50=QUOTE|52=20170117-11:13:44.461|56=live.theBroker.12345|57=any_string|268=12|279=0|269=1|278=7475|55=1|270=1.0691|271=2000000|279=0|269=1|278=7476|55=1|270=1.06911|271=3000000|279=0|269=1|278=7484|55=1|270=1.06931|271=34579000|279=0|269=1|278=7485|55=1|270=1.06908|271=1000000|279=0|269=1|278=7483|55=1|270=1.06906|271=500000|279=0|269=1|278=7482|55=1|270=1.06907|271=500000|279=0|269=1|278=7488|55=1|270=1.06909|271=3000000|279=0|269=0|278=7468|55=1|270=1.06898|271=500000|279=0|269=0|278=7467|55=1|270=1.06874|271=32371000|279=0|269=0|278=7457|55=1|270=1.06899|271=1000000|279=0|269=0|278=7478|55=1|270=1.06896|271=7000000|279=0|269=0|278=7477|55=1|270=1.06897|271=1500000|10=111|`

`8=FIX.4.4|9=376|35=X|34=3|49=CSERVER|50=QUOTE|52=20170117-11:13:44.555|56=live.theBroker.12345|57=any_string|268=8|279=0|269=0|278=7491|55=1|270=1.06897|271=1000000|279=0|269=0|278=7490|55=1|270=1.06898|271=1000000|279=0|269=0|278=7489|55=1|270=1.06874|271=32373000|279=0|269=1|278=7496|55=1|270=1.06931|271=34580000|279=2|278=7477|55=1|279=2|278=7468|55=1|279=2|278=7467|55=1|279=2|278=7484|55=1|10=192|`

### Market Data Snapshot/Full Refresh (MsgType(35)=W)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 262 | MDReqID | Yes | Any valid value | String | The ID of the market data request previously sent. |
| 55 | Symbol | Yes | Any valid value | Long | Instrument identificators are provided by Spotware. |
| 268 | NoMDEntries | Yes | Any valid value | Integer | The number of entries following. |
| 269 | MDEntryType | No | 0 or 1 | Char | The valid values are:<br>0 = Bid<br>1 = Offer<br>Required only when NoMDEntries (tag=268) > 0. |
| 299 | QuoteEntryID | No | Any valid value | String | A unique identification of the quote as a part of QuoteSet. |
| 270 | MDEntryPx | No | 1.2345 | Price | A price of the Market Data Entry. Required only when NoMDEntries (tag=268) > 0. |
| 271 | MDEntrySize | No | 500000 | Volume | Volume of the Market Data Entry. Required only when NoMDEntries (tag=268) > 0. |
| 278 | MDEntryID | No | Any valid value | String | A unique Market Data Entry identifier. |
| Standard Trailer | Yes | | | | |

### Market Data Incremental Refresh (MsgType(35)=X)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 262 | MDReqID | Yes | Any valid value | String | The ID of the market data request previously sent. |
| 268 | NoMDEntries | Yes | Any valid value | Integer | The number of entries following. This repeating group contains a list of all types of Market Data Entries the requester wants to receive. |
| 279 | MDUpdateAction | Yes | 0 or 2 | Char | A type of the Market Data update action. The valid values are:<br>0 = New<br>2 = Delete |
| 269 | MDEntryType | No | 0 or 1 | Char | The valid values are:<br>0 = Bid<br>1 = Offer |
| 278 | MDEntryID | Yes | Any valid value | String | An ID of the Market Data Entry. |
| 55 | Symbol | Yes | Any valid value | Long | Instrument identifiers are provided by Spotware. |
| 270 | MDEntryPx | No | 1.2345 | Price | Required only when MDUpdateAction (tag=279) = 0. |
| 271 | MDEntrySize | No | 10000 | Double | Required only when MDUpdateAction (tag=279) = 0. |
| Standard Trailer | Yes | | | | |

### New Order Single (MsgType(35)=D)

A New Order Single message has the following format.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 11 | ClOrdID | Yes | Any valid value | String | A unique identifier of the order allocated by the client. |
| 55 | Symbol | Yes | Any valid value | Long | Instrument identifiers are provided by Spotware. |
| 54 | Side | Yes | 1 or 2 | Integer | 1 = Buy<br>2 = Sell |
| 60 | TransactTime | Yes | Any valid value | Timestamp | Request time generated by the client. |
| 38 | OrderQty | Yes | Any valid value | Qty | The number of shares ordered. This represents the number of shares for equities or based on normal convention the number of contracts for options, futures, convertible bonds, etc. A maximum precision is 0.01. Prior to FIX 4.2, the type of this field was "Integer". |
| 40 | OrdType | Yes | 1, 2 or 3 | Char | 1 = Market, the order will be processed by the Immediate or Cancel (IOC) scheme (TimeInForce, tag=59).<br>2 = Limit, the order will be processed by the Good Till Cancel (GTC) scheme (TimeInForce, tag=59).<br>3 = Stop, the order will be processed by the Good Till Cancel (GTC) scheme (TimeInForce, tag=59). |
| 44 | Price | No | Any valid value | Price | The worst client price that the client will accept. Required only when OrdType (tag=40) = 2, in which case the order will not fill unless this price can be met. |
| 99 | StopPx | No | Any valid value | Price | A price that triggers the stop order. Required only when OrdType (tag=40) = 3, in which case the order will not fill unless this price can be met. |
| 59 | TimeInForce | No | 1, 3 or 6 | String | Deprecated, this value will be ignored. TimeInForce will be detected automatically depending on OrdType (tag=40) and ExpireTime (tag=126):<br>1 = Good Till Cancel (GTC), will be used only for limit and stop orders (OrdType, tag=40) only if ExpireTime (tag=126) is not defined.<br>3 = Immediate or Cancel (IOC), will be used only for market orders (OrdType, tag=40).<br>6 = Good Till Date (GTD), will be used only for limit and stop orders (OrdType, tag=40) only if ExpireTime (tag=126) is defined. |
| 126 | ExpireTime | No | 20140215-07:24:55 | Timestamp | Expire time in the "YYYYMMDD-HH:MM:SS" format. If assigned, the order will be processed by the GTD scheme (TimeInForce: GTD). |
| 721 | PosMaintRptID | No | Any valid value | String | A position ID where this order should be placed. If not set, a new position will be created and its ID will be returned in the Execution Report message. It can be specified only for hedged accounts. |
| 494 | Designation | No | Any valid value | String | A custom order label. |
| Standard Trailer | Yes | | | | |

See examples of New Order Single messages below.

Market order to new position

Request:
`8=FIX.4.4|9=143|35=D|49=live.theBroker.12345|56=CSERVER|34=77|52=20170117-10:02:14|50=any_string|57=TRADE|11=876316397|55=1|54=1|60=20170117-10:02:14|40=1|38=10000|10=010|`

Responses:
`8=FIX.4.4|9=197|35=8|34=77|49=CSERVER|50=TRADE|52=20170117-10:02:14.720|56=live.theBroker.12345|57=any_string|11=876316397|14=0|37=101|38=10000|39=0|40=1|54=1|55=1|59=3|60=20170117-10:02:14.591|150=0|151=10000|721=101|10=149|`

`8=FIX.4.4|9=206|35=8|34=78|49=CSERVER|50=TRADE|52=20170117-10:02:15.045|56=live.theBroker.12345|57=any_string|6=1.0674|11=876316397|14=10000|32=10000|37=101|38=10000|39=2|40=1|54=1|55=1|59=3|60=20170117-10:02:14.963|150=F|151=0|721=101|10=077|`

Market order to existing position

Request:
`8=FIX.4.4|9=151|35=D|49=live.theBroker.12345|56=CSERVER|34=80|52=20170117-10:02:55|50=any_string|57=TRADE|11=876316398|55=1|54=1|60=20170117-10:02:55|40=1|38=10000|721=101|10=120|`

Responses:
`8=FIX.4.4|9=197|35=8|34=80|49=CSERVER|50=TRADE|52=20170117-10:02:56.003|56=live.theBroker.12345|57=any_string|11=876316398|14=0|37=102|38=10000|39=0|40=1|54=1|55=1|59=3|60=20170117-10:02:55.984|150=0|151=10000|721=101|10=156|`

`8=FIX.4.4|9=207|35=8|34=81|49=CSERVER|50=TRADE|52=20170117-10:02:56.239|56=live.theBroker.12345|57=any_string|6=1.06735|11=876316398|14=10000|32=10000|37=102|38=10000|39=2|40=1|54=1|55=1|59=3|60=20170117-10:02:56.210|150=F|151=0|721=101|10=127`

Limit order to existing position

Request:
`8=FIX.4.4|9=162|35=D|49=live.theBroker.12345|56=CSERVER|34=89|52=20170117-10:06:22|50=any_string|57=TRADE|11=876316400|55=1|54=2|60=20170117-10:06:22|40=2|44=1.07162|38=50000|721=101|10=122|`

Response:
`8=FIX.4.4|9=208|35=8|34=90|49=CSERVER|50=TRADE|52=20170117-10:06:22.466|56=live.theBroker.12345|57=any_string|11=876316400|14=0|37=104|38=50000|39=0|40=2|44=1.07162|54=2|55=1|59=1|60=20170117-10:06:22.436|150=0|151=50000|721=101|10=149|`

Stop order to new position

Request:
`8=FIX.4.4|9=153|35=D|49=live.theBroker.12345|56=CSERVER|34=9|52=20170117-12:10:48|57=TRADE|50=any_string|11=876316418|55=1|54=1|60=20170117-12:10:48|40=3|38=50000|99=1.07148|10=249|`

Response:
`8=FIX.4.4|9=207|35=8|34=8|49=CSERVER|50=TRADE|52=20170117-12:10:48.400|56=live.theBroker.12345|57=any_string|11=876316418|14=0|37=205|38=50000|39=0|40=3|54=1|55=1|59=1|60=20170117-12:10:48.362|99=1.07148|150=0|151=50000|721=202|10=122|`

### Order Status Request (MsgType(35)=H)

An Order Status Request message is used by an institution to generate an order status message back from the trader. For a correct interaction, it is very important to have unique client order identifiers (ClOrdID) for all orders.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 11 | ClOrdID | Yes | Any valid value | String | A unique identifier of the order allocated by the client. |
| 54 | Side | No | 1 or 2 | Integer | 1 = Buy<br>2 = Sell |
| Standard Trailer | Yes | | | | |

See examples of Oder Status Request messages below.

Request:
`8=FIX.4.4|9=98|35=H|49=live.theBroker.12345|56=CSERVER|34=95|52=20170117-10:08:31|50=any_string|57=TRADE|11=876316400|10=191|`

Response:
`8=FIX.4.4|9=208|35=8|34=95|49=CSERVER|50=TRADE|52=20170117-10:08:31.819|56=live.theBroker.12345|57=any_string|11=876316400|14=0|37=104|38=50000|39=0|40=2|44=1.07162|54=2|55=1|59=1|60=20170117-10:06:22.436|150=0|151=50000|721=101|10=158|`

### Order Mass Status Request (MsgType(35)=AF)

An Order Mass Status Request message requests the status for orders matching the criteria specified within the request. The answer will be returned as a number of Execution Report messages (one for each order), or as a Business Message Reject message if no orders are found.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 584 | MassStatusReqID | Yes | Any valid value | String | A unique ID of the Mass Status Request as assigned by the client. |
| 585 | MassStatusReqType | Yes | Any valid value | Integer | 7 = Status for all orders.<br>Only the value 7 is currently supported. |
| 225 | IssueDate | No | Any valid value | String | If set, the response will contain only orders created before this date. |
| Standard Trailer | Yes | | | | |

See examples of Oder Mass Status Request messages below.

Request:
`8=FIX.4.4|9=117|35=AF|34=3|49=live.theBroker.12345|52=20170404-07:20:55.325|56=CSERVER|57=TRADE|225=20170404-07:20:44.582|584=mZzEY|585=7|10=065|`

Response:
`8=FIX.4.4|9=199|35=8|34=13|49=CSERVER|50=TRADE|52=20170404-07:20:55.333|56=live.theBroker.12345|14=0|37=635|38=100000|39=0|40=2|44=1.35265|54=2|55=1|59=1|60=20170404-07:20:44.582|150=I|151=100000|584=mZzEY|721=617|911=1|10=152|`

### Execution Report (MsgType(35)=8)

An Execution Report message for an accepted order has the following format.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 37 | OrderID | Yes | Any valid value | String | A cTrader order ID. |
| 11 | ClOrdID | No | Any valid value | String | A unique identifier of the order allocated by the client. |
| 911 | TotNumReports | No | Any valid value | Integer | The total number of reports returned in response to the Order Mass Status Request message. |
| 150 | ExecType | Yes | Any valid value | Char | 0 = New<br>4 = Canceled<br>5 = Replace<br>8 = Rejected<br>C = Expired<br>F = Trade<br>I = Order Status |
| 39 | OrdStatus | Yes | Any valid value | Char | 0 = New<br>1 = Partially filled<br>2 = Filled<br>8 = Rejected<br>4 = Cancelled (when the order is partially filled, Canceled is returned signifying (tag=151), LeavesQty is cancelled and will not be subsequently filled).<br>C = Expired |
| 55 | Symbol | No | Any valid value | Long | Instrument identifiers are provided by Spotware. |
| 54 | Side | No | 1 or 2 | Integer | 1 = Buy<br>2 = Sell |
| 60 | TransactTime | No | Any valid value | Timestamp | Execution time of a transaction represented by the Execution Report message (in UTC). |
| 6 | AvgPx | No | Any valid value | Integer | A price at which the deal was filled. For an IOC or GTD order, this is the Volume Weighted Average Price (VWAP) of the filled order. |
| 38 | OrderQty | No | Any valid value | Qty | This represents the number of shares for equities or based on normal convention the number of contracts for options, futures, convertible bonds, etc. Prior to FIX 4.2, the type of this field was "Integer". |
| 151 | LeavesQty | No | Any valid value | Qty | The number of orders still to be filled. Possible values are between 0 (fully filled) and OrderQty (partially filled). |
| 14 | CumQty | No | Any valid value | Qty | The total number of orders which have been filled. |
| 32 | LastQty | No | Any valid value | Qty | The bought/sold quantity of orders which have been filled on this (last) fill. |
| 40 | OrdType | No | 1 or 2 | Char | 1 = Market<br>2 = Limit |
| 44 | Price | No | Any valid value | Price | If supplied in a New Order Single message, it is echoed back in this Execution Report message. |
| 99 | StopPx | No | Any valid value | Price | If supplied in a New Order Single message, it is echoed back in this Execution Report message. |
| 59 | TimeInForce | No | 1, 3 or 6 | String | 1 = Good Till Cancel (GTC)<br>3 = Immediate or Cancel (IOC)<br>6 = Good Till Date (GTD) |
| 126 | ExpireTime | No | 20140215-07:24:55 | Timestamp | If supplied in a New Order Single message, it is echoed back in this Execution Report message. |
| 58 | Text | No | Any valid value | String | Where possible, a message will explain the Execution Report. |
| 103 | OrdRejReason | No | 0 | Integer | 0 = OrdRejReason.BROKER_EXCHANGE_OPTION |
| 721 | PosMaintRptID | No | Any valid value | String | A position ID. |
| 494 | Designation | No | Any valid value | String | A custom order label of the client. |
| 584 | MassStatusReqID | No | Any valid value | String | A unique ID of the mass status request as assigned by the client. |
| 1000 | AbsoluteTP | No | Any valid value | Price | An absolute price at which the take profit will be triggered. |
| 1001 | RelativeTP | No | Any valid value | Price | A distance in pips from the entry price at which the take profit will be triggered. |
| 1002 | AbsoluteSL | No | Any valid value | Price | An absolute price at which the stop loss will be triggered. |
| 1003 | RelativeSL | No | Any valid value | Price | A distance in pips from the entry price at which the stop loss will be triggered. |
| 1004 | TrailingSL | No | N or Y | Boolean | Indicates if the stop loss is trailing.<br>N = Stop loss is not trailing.<br>Y = Stop loss is trailing. |
| 1005 | TriggerMethodSL | No | Any valid value | Integer | An indicated trigger method of the stop loss.<br>1 = Stop loss will be triggered by the trade side.<br>2 = Stop loss will be triggered by the opposite side (ask for buy positions and by bid for sell positions).<br>3 = Stop loss will be triggered after two consecutive ticks according to the trade side.<br>4 = Stop loss will be triggered after two consecutive ticks according to the opposite side (the second ask tick for buy positions and the second bid tick for sell positions). |
| 1006 | GuaranteedSL | No | N or Y | Boolean | Indicates if the stop loss is guaranteed.<br>N = Stop loss is not guaranteed.<br>Y = Stop loss is guaranteed. |
| Standard Trailer | Yes | | | | |

See a New Order Single example at the end of the guide.

### Business Message Reject (MsgType(35)=j)

This message type is sent when the system is unable to process a subscription request or an order cannot be executed.

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 45 | RefSeqNum | No | Any valid value | Integer | MsgSeqNum (tag=34) of the rejected message. |
| 372 | RefMsgType | No | Any valid value | String | MsgType (tag=35) of the FIX message being referenced. |
| 379 | BusinessRejectRefID | No | Any valid value | String | Value of the business-level ID field in the message being referenced. Required unless the corresponding ID field was not specified. |
| 380 | BusinessRejectReason | Yes | 0 | Integer | A code to identify the reason for a Business Message Reject message.<br>0 = Other |
| 58 | Text | No | Any valid value | String | Where possible, a message to explain the reason for rejection. |
| Standard Trailer | Yes | | | | |

See an example of a Business Message Reject message below.

`8=FIX.4.4|9=149|35=j|34=2|49=CSERVER|52=20170105-06:36:00.912|56=live.theBroker.12345|57=any_string|58=Message to explain reason for rejection|379=u4Jr7Rr5t2VS7HSP|380=0|10=123|`

### Request for Positions (MsgType(35)=AN)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 710 | PosReqID | Yes | Any valid value | String | A unique request ID (set by the client). |
| 721 | PosMaintRptID | No | Any valid value | String | A position ID to request. If not set, all open positions will be returned. |
| Standard Trailer | Yes | | | | |

See examples of Request for Positions messages below.

Request:
`8=FIX.4.4|9=100|35=AN|49=live.theBroker.12345|56=CSERVER|34=99|52=20170117-10:09:54|50=any_string|57=TRADE|710=876316401|10=103|`

Response:
`8=FIX.4.4|9=163|35=AP|34=98|49=CSERVER|50=TRADE|52=20170117-10:09:54.076|56=live.theBroker.12345|57=any_string|55=1|710=876316401|721=101|727=1|728=0|730=1.06671|702=1|704=0|705=30000|10=182|`

### Position Report (MsgType(35)=AP)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 710 | PosReqID | Yes | Any valid value | String | An ID of the Request for Positions message. |
| 721 | PosMaintRptID | No | Any valid value | String | A position ID, which is not set if PosReqResult (tag=728) is not VALID_REQUEST. |
| 727 | TotalNumPosReports | Yes | Any valid value | String | The total count of Position Reports in a sequence when PosReqResult (tag=728) is VALID_REQUEST, otherwise = 0. |
| 728 | PosReqResult | Yes | 0 or 2 | String | 0 = Valid request<br>2 = No open positions that match the criteria are found. |
| 55 | Symbol | No | Any valid value | String | A symbol for which the current Position Report is prepared. It is not set if PosReqResult (tag=728) is not VALID_REQUEST. |
| 702 | NoPositions | No | 1 | String | 1 when PosReqResult (tag=728) is VALID_REQUEST, otherwise not set. |
| 704 | LongQty | No | Any valid value | String | Open volume of the position in case of the buy trade side, equal to 0 in case of the sell trade side. Not set if PosReqResult (tag=728) is not VALID_REQUEST. |
| 705 | ShortQty | No | Any valid value | String | Open volume of the position in case of the sell trade side, equal to 0 in case of the buy trade side. Not set if PosReqResult (tag=728) is not VALID_REQUEST. |
| 730 | SettlPrice | No | Any valid value | String | An average price of the opened volume in the current Position Report. |
| 1000 | AbsoluteTP | No | Any valid value | Price | An absolute price at which the take profit will be triggered. |
| 1002 | AbsoluteSL | No | Any valid value | Price | An absolute price at which the stop loss will be triggered. |
| 1004 | TrailingSL | No | Any valid value | Boolean | Indicates if the stop loss is trailing.<br>N = Stop loss is not trailing.<br>Y = Stop loss is trailing. |
| 1005 | TriggerMethodSL | No | Any valid value | Integer | An indicated trigger method of the stop loss.<br>1 = Stop loss will be triggered by the trade side.<br>2 = Stop loss will be triggered by the opposite side (ask for buy positions and bid for sell positions).<br>3 = Stop loss will be triggered after two consecutive ticks according to the trade side.<br>4 = Stop loss will be triggered after two consecutive ticks according to the opposite side (second ask tick for buy positions and second bid tick for sell positions). |
| 1006 | GuaranteedSL | No | Any valid value | Boolean | Indicates if the stop loss is guaranteed.<br>N = Stop loss is not guaranteed.<br>Y = Stop loss is guaranteed. |
| Standard Trailer | Yes | | | | |

### Order Cancel Request (MsgType(35)=F)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 41 | OrigClOrdID | Yes | Any valid value | String | A unique identifier of the order, which is going to be cancelled, allocated by the client. |
| 37 | OrderID | No | Any valid value | String | A unique ID of the order returned by cServer. |
| 11 | ClOrdID | Yes | Any valid value | String | A unique ID of the cancel request allocated by the client. |
| Standard Trailer | Yes | | | | |

See examples of Order Cancel Request messages below.

Request:
`8=FIX.4.4|9=115|35=F|34=2|49=live.theBroker.12345|50=Trade|52=20170721-13:41:21.694|56=CSERVER|57=TRADE|11=jR8dBPcZEQa9|41=n9Tm8x1AavO5|10=182|`

Response (success):
`8=FIX.4.4|9=221|35=8|34=3|49=CSERVER|50=TRADE|52=20170721-13:41:21.784|56=live.theBroker.12345|57=Trade|11=jR8dBPcZEQa9|14=0|37=641|38=100000|39=4|40=2|41=n9Tm8x1AavO5|44=1.499|54=1|55=1|59=1|60=20170721-13:41:21.760|150=4|151=100000|721=624|10=180|`

Response (failed):
`8=FIX.4.4|9=174|35=j|34=3|49=CSERVER|50=TRADE|52=20170721-13:41:21.856|56=live.theBroker.12345|57=Trade|58=ORDER_NOT_FOUND:Order with clientOrderId=n9Tm8x1AavO5 not found.|379=jR8dBPcZEQa9|380=0|10=075|`

### Order Cancel Reject (MsgType(35)=9)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 37 | OrderID | Yes | Any valid value | String | A unique identifier of the order which the system could not cancel. |
| 11 | ClOrdID | Yes | Any valid value | String | A unique identifier of the Order Cancel Request. |
| 41 | OrigClOrdID | No | Any valid value | String | A unique identifier of the order, which was attempted to be cancelled, allocated by the client. |
| 39 | OrdStatus | Yes | Any valid value | Char | 0 = New<br>1 = Partially filled<br>2 = Filled<br>8 = Rejected<br>4 = Cancelled (when the order is partially filled Canceled is returned signifying (tag=151), LeavesQty is cancelled and will not be subsequently filled).<br>C = Expired |
| 434 | CxlRejResponseTo | Yes | 1 or 2 | Char | 1 = Reject cancel order.<br>2 = Reject amend order from another terminal. |
| 58 | Text | No | Any valid value | String | An order is under execution. |
| Standard Trailer | Yes | | | | |

See an example of an Order Cancel Reject message below.

Response:
`8=FIX.4.4|9=156|35=9|34=3|49=CSERVER|50=TRADE|52=20181024-12:35:02.896|56=live.theBroker.12345|57=Trade|11=gBljx7YOg5jY|37=629|39=0|41=FdXLfS0tTyUL|58=Order is under execution|434=1|10=109|`

### Order Cancel/Replace Request (MsgType(35)=G)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 41 | OrigClOrdID | Yes | Any valid value | String | A unique identifier of the order, which is going to be amended, allocated by the client. |
| 37 | OrderID | No | Any valid value | String | A unique ID of the original order, which is going to be amended, allocated by the server. A preferable method to use. |
| 11 | ClOrdID | Yes | Any valid value | String | A unique ID of the amend request allocated by the client. |
| 38 | OrderQty | Yes | Any valid value | Qty | An existing or newly specified quantity for replacing the old value. |
| 44 | Price | No | Any valid value | Price | An existing or newly specified limit price for replacing the old value. Valid only for limit orders. |
| 99 | StopPx | No | Any valid value | Price | An existing or newly specified stop price for replacing the old value. Valid only for stop orders. |
| 126 | ExpireTime | No | 20140215-07:24:55 | Timestamp | Existing or newly specified expiration time. Valid only for pending orders. |
| Standard Trailer | Yes | | | | |

See examples of Order Cancel/Replace Request messages below.

Request:
`8=FIX.4.4|9=123|35=G|34=3|49=live.theBroker.12345|50=Trade|52=20170721-13:42:17.680|56=CSERVER|57=TRADE|11=Is03AvsknNYK|38=5000|41=n9Tm8x1AavO5|44=1.1|10=010|`

Response (success):
`8=FIX.4.4|9=192|35=8|34=3|49=CSERVER|50=TRADE|52=20170721-13:42:18.784|56=live.theBroker.12345|57=Trade|11=Is03AvsknNYK|14=0|37=629|38=5000|39=0|40=2|44=1.1|54=1|55=1|59=1|60=20170721-13:42:18.760|150=5|151=5000|721=624|10=150|`

Response (failed):
`8=FIX.4.4|9=171|35=j|34=3|49=CSERVER|50=TRADE|52=20170721-13:42:18.784|56=live.theBroker.12345|57=Trade|58=ORDER_NOT_FOUND:Order with orderId:4429421711699105367 isn't found|379=NXek3EzJvMme|380=0|10=245|`

### Market Data Request Reject (MsgType(35)=Y)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 262 | MDReqID | Yes | Any valid value | String | Must refer to MDReqID (tag=262) of the request. |
| 281 | MDReqRejReason | No | Any valid value | Integer | 0 = Unknown symbol<br>4 = Unsupported SubscriptionRequestType (tag=263)<br>5 = Unsupported MarketDepth (tag=264) |
| Standard Trailer | Yes | | | | |

See examples of Market Data Request Reject messages below.

Request:
`8=FIX.4.4|9=148|35=V|34=2|49=live.theBroker.12345|50=Quote|52=20170920-09:52:13.032|56=CSERVER|57=QUOTE|262=CS8260:sXlXex|263=1|264=0|265=1|146=1|55=CS8260|267=2|269=0|269=1|10=129|`

Reject:
`8=FIX.4.4|9=164|35=Y|34=2|49=CSERVER|50=QUOTE|52=20170920-09:52:13.036|56=live.theBroker.12345|57=Quote|58=INVALID_REQUEST: Expected numeric symbolId, but got CS8260|262=CS8260:sXlXex|281=0|10=236|`

Request:
`8=FIX.4.4|9=136|35=V|34=6|49=live.theBroker.12345|50=Quote|52=20170920-09:52:13.199|56=CSERVER|57=QUOTE|262=EwOhiWvMdCpc|263=1|264=3|146=1|55=1|267=2|269=0|269=1|10=182|`

Reject:
`8=FIX.4.4|9=157|35=Y|34=6|49=CSERVER|50=QUOTE|52=20170920-09:52:13.201|56=live.theBroker.12345|57=Quote|58=INVALID_REQUEST: MarketDepth should be either 0 or 1|262=EwOhiWvMdCpc|281=5|10=088|`

### Security List Request (MsgType(35)=x)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 320 | SecurityReqID | Yes | Any valid value | String | A unique ID of the Security Definition Request. |
| 559 | SecurityListRequestType | Yes | 0 | Integer | The type of a Security List Request being made. Supported only 0 = Symbol (tag=55). |
| 55 | Symbol | No | Any valid value | Integer | An ID for resolving the symbol name. |
| Standard Trailer | Yes | | | | |

See examples of Security List Request messages below.

Request:
`8=FIX.4.4|9=107|35=x|34=3|49=live.theBroker.12345|50=Trade|52=20180427-12:24:27.106|56=CSERVER|57=TRADE|55=39|320=ILCea0JkdQEm|559=0|10=248|`

Response:
`8=FIX.4.4|9=158|35=y|34=3|49=CSERVER|50=TRADE|52=20180427-12:24:27.107|56=live.theBroker.12345|57=Trade|320=ILCea0JkdQEm|322=responce:ILCea0JkdQEm|560=0|146=1|55=39|1007=NZDCHF|1008=4|10=088|`

### Security List (MsgType(35)=y)

| Tag | Field name | Required | Value | FIX format | Comments |
|---|---|---|---|---|---|
| Standard Header | Yes | | | | |
| 320 | SecurityReqID | Yes | Any valid value | String | A unique ID of the Security Definition Request. |
| 322 | SecurityResponseID | Yes | Any valid value | String | A unique ID of the Security List response. |
| 560 | SecurityRequestResult | Yes | 0 | Integer | Results returned to the Security Request message. The valid values are:<br>0 = Valid request.<br>1 = Invalid or unsupported request.<br>2 = No instruments that match the selection criteria are found.<br>3 = Not authorised to retrieve instrument data.<br>4 = Instrument data temporarily unavailable.<br>5 = Request for instrument data not supported. |
| 146 | NoRelatedSym | No | Any valid value | Integer | Specifies the number of repeating symbols (instruments). |
| 55 | Symbol | No | Any valid value | Integer | Instrument identifiers are provided by Spotware. |
| 1007 | SymbolName | No | Any valid value | String | A symbol name. |
| 1008 | SymbolDigits | No | Any valid value | Integer | Symbol digits. Possible values from 0 to 5. |
| Standard Trailer | Yes | | | | |

See an example of a Security List message below.

Response:
`8=FIX.4.4|9=3977|35=y|34=2|49=CSERVER|50=TRADE|52=20180426-12:07:37.816|56=live.theBroker.12345|57=Trade|320=Sxo2Xlb1jzJC|322=responce:Sxo2Xlb1jzJC|560=0|146=143|55=1|1007=EURUSD|1008=5|55=2|1007=GBPUSD|1008=5|55=3|1007=EURJPY|1008=3|55=4|1007=USDJPY|1008=3|55=5|1007=AUDUSD|1008=5|55=6|1007=USDCHF|1008=5|55=7|1007=GBPJPY|1008=3|55=8|1007=USDCAD|1008=5|55=9|1007=EURGBP|1008=5|55=10|1007=EURCHF|1008=5|55=11|1007=AUDJPY|1008=2|55=12|1007=NZDUSD|1008=5|55=13|1007=CHFJPY|1008=2|55=14|1007=EURAUD|1008=4|55=15|1007=CADJPY|1008=2|55=16|1007=GBPAUD|1008=4|55=17|1007=EURCAD|1008=4|55=10001|1007=USDCFDSAX|1008=5|55=18|1007=AUDCAD|1008=4|55=10002|1007=CD3295|1008=5|55=19|1007=GBPCAD|1008=4|55=10003|1007=DU3295|1008=5|55=20|1007=AUDNZD|1008=4|55=10004|1007=CS5965|1008=2|55=21|1007=NZDJPY|1008=2|55=10005|1007=CS6014_3|1008=5|55=22|1007=USDNOK|1008=4|55=10006|1007=DU6014_3|1008=5|55=23|1007=AUDCHF|1008=4|55=10007|1007=CS6014_4|1008=5|55=24|1007=USDMXN|1008=4|55=10008|1007=DU6014_4|1008=5|55=25|1007=GBPNZD|1008=4|55=10009|1007=CS5953|1008=5|55=26|1007=EURNZD|1008=4|55=10010|1007=CS6407_01_EURUSD|1008=5|55=27|1007=CADCHF|1008=4|55=10011|1007=CS6407_01_GBPUSD|1008=5|55=28|1007=USDSGD|1008=5|55=10012|1007=CS6407_02_EURUSD|1008=5|55=29|1007=USDSEK|1008=4|55=10013|1007=CS6407_03_EURUSD|1008=5|55=30|1007=NZDCAD|1008=4|55=31|1007=EURSEK|1008=4|55=10015|1007=CS7847_01_EURUSD|1008=5|55=32|1007=GBPSGD|1008=4|55=10016|1007=CS7847_01_GBPUSD|1008=5|55=33|1007=EURNOK|1008=4|55=10017|1007=CS7847_02_EURUSD|1008=5|55=34|1007=EURHUF|1008=2|55=10018|1007=CS7847_03_EURUSD|1008=5|55=35|1007=USDPLN|1008=4|55=10019|1007=CS7847_04_GBPUSD|1008=5|55=36|1007=USDDKK|1008=4|55=10020|1007=CS9004S|1008=2|10=096|`
