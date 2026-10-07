# OTA ผ่าน MQTT ทั้งหมด — 1.0.0-ota.5

## วิธีใช้งาน

1. นำ API รุ่นใหม่ขึ้นเครื่อง .133 รวม appsettings.json แล้วรัน `sudo docker compose up -d --build sfom-api` ในโฟลเดอร์ webapi
2. API เชื่อม MQTT ภายในไป 10.1.220.131:21883 ด้วยบัญชี Ota:Mqtt เดิม ตรวจ log ว่ามี `OTA MQTT service connected and subscribed`
3. บอร์ดใช้ broker.ntplc.co.th:21883 เดิม ไม่เรียก HTTP 8080/5000 อีกต่อไป แฟลช firmware 1.0.0-ota.5 ผ่าน USB ครั้งแรกด้วย PlatformIO Upload
4. บอร์ดที่ยังไม่จับคู่จะปรากฏในหน้า /ems/ota ให้ตรวจ MAC แล้วกดรับบอร์ด; บอร์ดที่เคยจับคู่แล้วเก็บกุญแจเดิมและรายงานเวอร์ชันต่อได้
5. เลือก firmware.bin บนเว็บตามเดิม กดอัปโหลด เลือกบอร์ด แล้วสั่งอัปเดต/คืนรุ่นโรงงาน

ไม่ต้องใช้ gateway 8080 สำหรับบอร์ดรุ่นนี้ ไม่ต้องเปลี่ยนเว็บหรือเพิ่ม SQL จากรุ่น .4 แต่ยังต้องมีตาราง OTA เดิมรวม ota_enrollments และไฟล์ OTAData/enrollment-private.pem บน API .133 เพื่อรับบอร์ดใหม่

## สิทธิ์ MQTT ที่ต้องมี

- บอร์ด publish: ems/{MAC}/ota/rpc/request
- บอร์ด subscribe: ems/{MAC}/ota/rpc/response และ ems/{MAC}/ota/notify
- API subscribe: ems/+/ota/rpc/request
- API publish: ems/+/ota/rpc/response และ ems/+/ota/notify
- ข้อความ OTA ห้าม retained; เมตรและ OTA มี MQTT client ID คนละตัว ต้องอนุญาตทั้งสอง connection

API/บอร์ดต้องมี username/password ตาม broker ตั้งไว้ การส่งมิเตอร์สำเร็จไม่ได้ยืนยันสิทธิ์ subscribe topic OTA

## การรับไฟล์

บอร์ดพักอ่านเซนเซอร์ช่วงติดตั้ง ขอทีละ 1024 bytes ตามลำดับ ตรวจ HMAC ทุกคำตอบ ขอชิ้นเดิมซ้ำได้สูงสุด 3 ครั้ง ครั้งละ 15 วินาที และตรวจ SHA-256 ทั้งไฟล์ก่อนเปิดใช้/รีบูตหนึ่งครั้ง
หากเน็ตหลุดหรือส่งซ้ำไม่สำเร็จ ยกเลิก partial image ใช้ firmware เดิมต่อและรายงานล้มเหลวเมื่อเชื่อมได้ ต้องสั่งงานใหม่เพื่อเริ่มใหม่ ไม่ resume หลัง reboot
จำกัดเวลารวม 10 นาทีโดยตรวจระหว่างชิ้น (คำขอปัจจุบันอาจเพิ่มได้สูงสุด 45 วินาที) และเว้น 25 ms ต่อชิ้น ลดภาระ broker ควรเริ่มอัปเดตทีละบอร์ด

เมื่อไม่มีงานตรวจผ่าน MQTT ทุก 1 ชั่วโมง และเมื่อ notify/reconnect โดยไม่ถี่กว่า 1 นาที

## คืนค่าโรงงาน

ใช้รุ่นที่รองรับ MQTT OTA เช่น .5 ที่ผ่านการทดสอบจริง กำหนดเป็น factory ผ่านหน้าเว็บได้ รุ่นเก่า .3/.4 ที่ใช้ HTTP จะถูก API ปฏิเสธเมื่อส่งให้บอร์ด MQTT-only เพื่อป้องกันขาดการติดต่อ
ไม่ใช่ bootloader rollback: กรณี firmware บูตไม่ขึ้นยังต้องกู้ผ่าน USB; ไม่ล้าง NVS/key/เครือข่าย

## ข้อความตรวจสอบ

- บอร์ด: OTA automatic MQTT enrollment ready / OTA waiting for approval on web page
- บอร์ด: OTA MQTT poll failed; retry in one minute -> ตรวจ broker ACL และ log API
- API: OTA MQTT service connected and subscribed
- API: OTA MQTT request/connection failed -> ตรวจ broker, ตาราง OTA, private key, และฐานข้อมูล

การเปลี่ยนนี้คงคำสั่งอ่านมิเตอร์ รูปแบบ JSON และ topic เดิมทั้งหมด ชุดทดสอบใช้ฐานข้อมูลจำลอง/loopback เท่านั้น ต้องทดสอบ LAN/Wi-Fi, ข้อมูลมิเตอร์และไฟดับบนบอร์ดจริงก่อนใช้หน้างาน
