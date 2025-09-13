const puppeteer = require('puppeteer');
const path = require('path');
(async ()=>{
  const file = 'file://' + path.resolve(__dirname,'test_olsr.html');
  const browser = await puppeteer.launch({ args: ['--no-sandbox','--disable-setuid-sandbox']});
  const page = await browser.newPage();
  page.on('console', msg => { console.log('PAGE_CONSOLE:', msg.text()); });
  await page.goto(file, { waitUntil: 'load' });
  await page.waitForTimeout(200);
  await browser.close();
})();
