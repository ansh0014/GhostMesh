package gossip
import(
"crypto/rand"
	"encoding/binary"
	"net"
	"sync"
	"time"
)
type Message struct{
	UUID [16] byte
	Sender string
	Timestamp time.time
	Text string

}
type DeduplicationCache struct{
	mu sync.Mutex
	seen map[[16]byte] struct{}
	ring [] [16] byte
	size int
	index int
}
func NewDeduplicationCache(size int)  *DeduplicationCache{
	return &DeduplicationCache{
		seen: make(map[[16]byte] struct{})
		ring: make([] [16]byte, size)
		size: size,
		
	}
	
}
func (c* DeduplicationCache) IsSeen(uuid [16]byte) bool{
	c.mu.Lock()
	defer c.mu.Unlock()
	_, ok:= c.seen[uuid]
	return ok
}
func (c *DeduplicationCache)
MarkSeen(uuid [16]byte){
	c.mu.Lock()
	defer c.mu.Unlock()
	
	if len(c.seen)>=c.size{
		old:=c.ring[c.index]
		delete(c.seen,old)
	}
	c.ring[c.index]=uuid
	c.index=(c.index+1)%c.size
	c.seen[uuid]=struct{}{}

}
type Router struct{
	name string
	conn net.conn
	cache *DeduplicationCache
	msgChan chan Message
	mu sync.Mutex

	
}
func NewRouter(name string, conn net.Conn) *Router{
	return &Router{
		name : name,
		conn: conn,
		cache: NewDeduplicationCache(1000),
		msgChan: make(chan Message,64),
	}
}
func (r* Router) Send(text string) error{
	var  uuid [16]byte
	if _,err:=rand.Read(uuid[:]); err!=nil{
		return err
	}
	r.cache.MarkSeen(uuid)
	ts:=time.Now().UnixMilli()
	senderBytes:=[] byte(r.name)
	textBytes:=[] byte(text)
	
	frame := make([]byte,0, 16+8+1+len(senderBytes)+2+len(textBytes))
	frame=append(frame,uuid[:]...)
	frame=binary.LittleEndian.AppendUint64(frame, uint64(ts))
	frame=append(frame,byte(len(senderBytes)))
	
	frame=append(frame,senderBytes...)
	
	frame=binary.LittleEndian.AppendUint64(frame,unit16(len(textBytes)) )
	frame=append(frame,textBytes ...)
	out:=make([]byte,4+len(frame))
	binary.LittleEndian.PutUint32(out, unit32(len(frame)))
	copy(out[4:],frame)
	r.mu.Lock()
	defer r.mu.Unlock()
	_,err:=r.conn.Write(out)
	return err

}
func (r* Router) Listen(){
	for{
		lenBuf:=make([]byte,4)
		if_,err:=readExact(r.conn,lenBuf);err!=nil{
			close(r.msgChan)
			return
		}
		frameLen:=binary.LittleEndian.unit32(lenBuf)
		if frameLen==0 || frameLen>65536
		{
			continue
		}
		frame:=make([]byte,frameLen)
		if _, err:=readExact(r.conn)
	}
}